#include "postgres.h"

#include "bloompg/column_store.h"

#include "access/htup_details.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

typedef struct BloomPGColumnChunk
{
	BloomPGColumnVector *vectors;
	uint16	   *selection;
	uint16		count;
	uint16		selected_count;
	uint64		selected_base;
} BloomPGColumnChunk;

struct BloomPGColumnStore
{
	MemoryContext memory_context;
	MemoryContext column_context;
	ResourceOwner runtime_owner;
	TupleDesc	descriptor;
	int			natts;
	BloomPGColumnChunk **chunks;
	uint32		nchunks;
	uint32		chunk_capacity;
	uint32		peak_chunks;
	uint64		rows;
	uint64		live_rows;
	bool		selection_bases_dirty;

	Size		memory_limit;
	Size		memory_bytes;
	Size		peak_memory_bytes;
	uint64		spill_rows;
	Tuplestorestate *spill;
	uint64	  **spill_masks;
	uint32		spill_mask_count;
	uint32		spill_mask_capacity;
	Size		spill_index_bytes;

	uint64		varlen_values;
	Size		varlen_bytes;
	uint64		selection_rewrites;
	uint64		selection_rows_examined;
	uint64		selection_rows_rejected;
	uint64		slot_reconstructions;
};

static void
bloompg_column_store_account(BloomPGColumnStore *store, Size bytes)
{
	store->memory_bytes = add_size(store->memory_bytes, bytes);
	store->peak_memory_bytes = Max(store->peak_memory_bytes,
		store->memory_bytes);
}

static Size
bloompg_column_vector_width(BloomPGVectorPhysicalType physical_type)
{
	switch (physical_type)
	{
		case BLOOMPG_VECTOR_BYVAL_1:
			return sizeof(uint8);
		case BLOOMPG_VECTOR_BYVAL_2:
			return sizeof(uint16);
		case BLOOMPG_VECTOR_BYVAL_4:
			return sizeof(uint32);
		case BLOOMPG_VECTOR_BYVAL_8:
			return sizeof(uint64);
		case BLOOMPG_VECTOR_BYREF:
			return sizeof(uint32);
	}
	pg_unreachable();
}

static BloomPGVectorPhysicalType
bloompg_column_vector_physical_type(Form_pg_attribute attribute)
{
	if (!attribute->attbyval)
		return BLOOMPG_VECTOR_BYREF;
	switch (attribute->attlen)
	{
		case 1:
			return BLOOMPG_VECTOR_BYVAL_1;
		case 2:
			return BLOOMPG_VECTOR_BYVAL_2;
		case 4:
			return BLOOMPG_VECTOR_BYVAL_4;
		case 8:
			return BLOOMPG_VECTOR_BYVAL_8;
		default:
			elog(ERROR, "unsupported bloompg by-value width %d",
				 attribute->attlen);
	}
	pg_unreachable();
}

static void
bloompg_column_store_ensure_chunk_array(BloomPGColumnStore *store)
{
	uint32		old_capacity;
	uint32		new_capacity;
	Size		old_bytes;
	Size		new_bytes;

	if (store->nchunks < store->chunk_capacity)
		return;
	old_capacity = store->chunk_capacity;
	new_capacity = old_capacity == 0 ? 16 : old_capacity * 2;
	if (new_capacity <= old_capacity ||
		new_capacity > (uint32) (MaxAllocSize /
			sizeof(BloomPGColumnChunk *)))
		elog(ERROR, "bloompg materialization has too many vector chunks");
	old_bytes = (Size) old_capacity * sizeof(BloomPGColumnChunk *);
	new_bytes = (Size) new_capacity * sizeof(BloomPGColumnChunk *);
	if (store->chunks == NULL)
		store->chunks = MemoryContextAlloc(store->memory_context, new_bytes);
	else
		store->chunks = repalloc(store->chunks, new_bytes);
	MemSet(store->chunks + old_capacity, 0, new_bytes - old_bytes);
	store->chunk_capacity = new_capacity;
	bloompg_column_store_account(store, new_bytes - old_bytes);
}

static void
bloompg_column_vector_initialize(BloomPGColumnStore *store,
	BloomPGColumnVector *vector, Form_pg_attribute attribute)
{
	Size		width;
	Size		data_bytes;
	Size		validity_bytes = sizeof(uint64) * BLOOMPG_VALIDITY_WORDS;

	vector->type_oid = attribute->atttypid;
	vector->typlen = attribute->attlen;
	vector->physical_type = bloompg_column_vector_physical_type(attribute);
	width = bloompg_column_vector_width(vector->physical_type);
	data_bytes = mul_size(width, BLOOMPG_VECTOR_SIZE);
	vector->data = palloc0(data_bytes);
	vector->validity = palloc0(validity_bytes);
	bloompg_column_store_account(store, add_size(data_bytes, validity_bytes));
}

static BloomPGColumnChunk *
bloompg_column_store_new_chunk(BloomPGColumnStore *store)
{
	BloomPGColumnChunk *chunk;
	MemoryContext old_context;
	int			attribute;

	if (store->descriptor == NULL || store->natts <= 0)
		elog(ERROR, "invalid bloompg vector descriptor");
	bloompg_column_store_ensure_chunk_array(store);
	old_context = MemoryContextSwitchTo(store->column_context);
	chunk = palloc0(sizeof(BloomPGColumnChunk));
	chunk->vectors = palloc0_array(BloomPGColumnVector, store->natts);
	chunk->selection = palloc_array(uint16, BLOOMPG_VECTOR_SIZE);
	bloompg_column_store_account(store, sizeof(BloomPGColumnChunk));
	bloompg_column_store_account(store,
		mul_size(sizeof(BloomPGColumnVector), store->natts));
	bloompg_column_store_account(store,
		mul_size(sizeof(uint16), BLOOMPG_VECTOR_SIZE));
	for (attribute = 0; attribute < store->natts; attribute++)
		bloompg_column_vector_initialize(store, &chunk->vectors[attribute],
			TupleDescAttr(store->descriptor, attribute));
	MemoryContextSwitchTo(old_context);
	store->chunks[store->nchunks++] = chunk;
	store->peak_chunks = Max(store->peak_chunks, store->nchunks);
	store->selection_bases_dirty = true;
	return chunk;
}

static void
bloompg_column_vector_reserve_heap(BloomPGColumnStore *store,
	BloomPGColumnVector *vector, Size required)
{
	uint32		old_capacity = vector->heap_capacity;
	uint32		new_capacity;

	if (required <= old_capacity)
		return;
	if (required > UINT32_MAX || required > MaxAllocSize)
		elog(ERROR, "bloompg vector heap exceeds PostgreSQL allocation limits");
	new_capacity = old_capacity == 0 ? 4096 : old_capacity;
	while ((Size) new_capacity < required)
	{
		uint32 limit = (uint32) Min((Size) UINT32_MAX,
			(Size) MaxAllocSize);

		if (new_capacity > limit / 2)
		{
			new_capacity = (uint32) required;
			break;
		}
		new_capacity *= 2;
	}
	if (vector->heap == NULL)
		vector->heap = palloc(new_capacity);
	else
		vector->heap = repalloc(vector->heap, new_capacity);
	vector->heap_capacity = new_capacity;
	bloompg_column_store_account(store, (Size) new_capacity - old_capacity);
}

static void
bloompg_column_vector_append(BloomPGColumnStore *store,
	BloomPGColumnVector *vector, uint32 row, Datum value, bool is_null)
{
	if (is_null)
		return;
	vector->validity[row / BLOOMPG_VALIDITY_BITS] |=
		UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS);
	switch (vector->physical_type)
	{
		case BLOOMPG_VECTOR_BYVAL_1:
			((uint8 *) vector->data)[row] = (uint8) value;
			break;
		case BLOOMPG_VECTOR_BYVAL_2:
			((uint16 *) vector->data)[row] = (uint16) value;
			break;
		case BLOOMPG_VECTOR_BYVAL_4:
			((uint32 *) vector->data)[row] = (uint32) value;
			break;
		case BLOOMPG_VECTOR_BYVAL_8:
			((uint64 *) vector->data)[row] = (uint64) value;
			break;
		case BLOOMPG_VECTOR_BYREF:
		{
			const char *source;
			struct varlena *detoasted = NULL;
			Size		size;
			Size		offset = MAXALIGN(vector->heap_used);
			Size		required;

			if (vector->typlen == -1)
			{
				detoasted = PG_DETOAST_DATUM_PACKED(value);
				source = (const char *) detoasted;
				size = VARSIZE_ANY(detoasted);
			}
			else if (vector->typlen == -2)
			{
				source = DatumGetCString(value);
				size = strlen(source) + 1;
			}
			else
			{
				Assert(vector->typlen > 0);
				source = (const char *) DatumGetPointer(value);
				size = (Size) vector->typlen;
			}
			required = add_size(offset, size);
			bloompg_column_vector_reserve_heap(store, vector, required);
			memcpy(vector->heap + offset, source, size);
			((uint32 *) vector->data)[row] = (uint32) offset;
			vector->heap_used = (uint32) required;
			store->varlen_values++;
			store->varlen_bytes = add_size(store->varlen_bytes, size);
			if (detoasted != NULL &&
				(Pointer) detoasted != DatumGetPointer(value))
				pfree(detoasted);
			break;
		}
	}
}

static void
bloompg_column_store_fill_memory_slot(BloomPGColumnStore *store,
	uint32 chunk_index, uint32 row, TupleTableSlot *slot)
{
	BloomPGColumnChunk *chunk;
	int			attribute;

	Assert(chunk_index < store->nchunks && row < BLOOMPG_VECTOR_SIZE);
	chunk = store->chunks[chunk_index];
	Assert(row < chunk->count);
	ExecClearTuple(slot);
	for (attribute = 0; attribute < store->natts; attribute++)
	{
		BloomPGColumnVector *vector = &chunk->vectors[attribute];

		slot->tts_isnull[attribute] =
			!bloompg_column_vector_is_valid(vector, row);
		if (!slot->tts_isnull[attribute])
			slot->tts_values[attribute] =
				bloompg_column_vector_get_datum(vector, row);
		else
			slot->tts_values[attribute] = (Datum) 0;
	}
	ExecStoreVirtualTuple(slot);
	store->slot_reconstructions++;
}

static void
bloompg_column_store_ensure_spill_mask(BloomPGColumnStore *store,
	uint32 chunk_index)
{
	if (chunk_index >= store->spill_mask_capacity)
	{
		uint32 old_capacity = store->spill_mask_capacity;
		uint32 new_capacity = old_capacity == 0 ? 16 : old_capacity;
		Size old_bytes;
		Size new_bytes;

		while (new_capacity <= chunk_index)
		{
			if (new_capacity > (uint32) (MaxAllocSize /
					(2 * sizeof(uint64 *))))
				elog(ERROR, "bloompg spill index has too many chunks");
			new_capacity *= 2;
		}
		old_bytes = (Size) old_capacity * sizeof(uint64 *);
		new_bytes = (Size) new_capacity * sizeof(uint64 *);
		if (store->spill_masks == NULL)
			store->spill_masks = MemoryContextAllocZero(store->memory_context,
				new_bytes);
		else
		{
			store->spill_masks = repalloc(store->spill_masks, new_bytes);
			MemSet(store->spill_masks + old_capacity, 0,
				new_bytes - old_bytes);
		}
		store->spill_mask_capacity = new_capacity;
		store->spill_index_bytes = add_size(store->spill_index_bytes,
			new_bytes - old_bytes);
	}
	while (store->spill_mask_count <= chunk_index)
	{
		store->spill_masks[store->spill_mask_count] =
			MemoryContextAllocZero(store->memory_context,
				sizeof(uint64) * BLOOMPG_VALIDITY_WORDS);
		store->spill_mask_count++;
		store->spill_index_bytes = add_size(store->spill_index_bytes,
			sizeof(uint64) * BLOOMPG_VALIDITY_WORDS);
	}
}

static void
bloompg_column_store_spill_set_active(BloomPGColumnStore *store,
	uint64 row_index)
{
	uint64		chunk64 = row_index / BLOOMPG_VECTOR_SIZE;
	uint32		row = (uint32) (row_index % BLOOMPG_VECTOR_SIZE);
	uint32		chunk_index;

	if (chunk64 > UINT32_MAX)
		elog(ERROR, "bloompg spill index exceeds its chunk address space");
	chunk_index = (uint32) chunk64;
	bloompg_column_store_ensure_spill_mask(store, chunk_index);
	store->spill_masks[chunk_index][row / BLOOMPG_VALIDITY_BITS] |=
		UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS);
}

static bool
bloompg_column_store_spill_is_active(const BloomPGColumnStore *store,
	uint64 row_index)
{
	uint64		chunk64 = row_index / BLOOMPG_VECTOR_SIZE;
	uint32		row = (uint32) (row_index % BLOOMPG_VECTOR_SIZE);

	if (chunk64 >= store->spill_mask_count)
		return false;
	return (store->spill_masks[chunk64][row / BLOOMPG_VALIDITY_BITS] &
		(UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS))) != 0;
}

static Tuplestorestate *
bloompg_column_store_begin_spill(BloomPGColumnStore *store)
{
	MemoryContext old_context = CurrentMemoryContext;
	ResourceOwner old_owner = CurrentResourceOwner;
	Tuplestorestate *volatile result = NULL;

	PG_TRY();
	{
		MemoryContextSwitchTo(store->memory_context);
		CurrentResourceOwner = store->runtime_owner;
		result = tuplestore_begin_heap(true, false, work_mem);
	}
	PG_FINALLY();
	{
		MemoryContextSwitchTo(old_context);
		CurrentResourceOwner = old_owner;
	}
	PG_END_TRY();
	return (Tuplestorestate *) result;
}

static void
bloompg_column_store_migrate_to_spill(BloomPGColumnStore *store)
{
	TupleTableSlot *slot;
	uint32		chunk_index;

	Assert(store->spill == NULL);
	Assert(store->descriptor != NULL);
	store->spill_rows = store->rows;
	store->spill = bloompg_column_store_begin_spill(store);
	for (chunk_index = 0; chunk_index < store->nchunks; chunk_index++)
	{
		BloomPGColumnChunk *chunk = store->chunks[chunk_index];
		uint32		selected;

		bloompg_column_store_ensure_spill_mask(store, chunk_index);
		for (selected = 0; selected < chunk->selected_count; selected++)
		{
			uint32 row = chunk->selection[selected];

			store->spill_masks[chunk_index][row / BLOOMPG_VALIDITY_BITS] |=
				UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS);
		}
	}
	slot = MakeSingleTupleTableSlot(store->descriptor, &TTSOpsVirtual);
	for (chunk_index = 0; chunk_index < store->nchunks; chunk_index++)
	{
		BloomPGColumnChunk *chunk = store->chunks[chunk_index];
		uint32		row;

		for (row = 0; row < chunk->count; row++)
		{
			bloompg_column_store_fill_memory_slot(store, chunk_index, row, slot);
			tuplestore_puttupleslot(store->spill, slot);
		}
	}
	ExecDropSingleTupleTableSlot(slot);
	MemoryContextReset(store->column_context);
	if (store->chunks != NULL)
	{
		pfree(store->chunks);
		store->chunks = NULL;
	}
	store->nchunks = 0;
	store->chunk_capacity = 0;
	store->memory_bytes = store->spill_index_bytes;
	store->selection_bases_dirty = false;
}

static void
bloompg_column_store_rebuild_selected_bases(BloomPGColumnStore *store)
{
	uint64		base = 0;
	uint32		chunk_index;

	if (!store->selection_bases_dirty || store->spill != NULL)
		return;
	for (chunk_index = 0; chunk_index < store->nchunks; chunk_index++)
	{
		BloomPGColumnChunk *chunk = store->chunks[chunk_index];

		chunk->selected_base = base;
		base += chunk->selected_count;
	}
	if (base != store->live_rows)
		elog(ERROR, "bloompg vector selection cardinality changed");
	store->selection_bases_dirty = false;
}

static BloomPGColumnChunk *
bloompg_column_store_find_selected(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan, uint64 logical_row, uint32 *chunk_index,
	uint32 *physical_row)
{
	uint32		low = 0;
	uint32		high = store->nchunks;
	BloomPGColumnChunk *chunk;

	bloompg_column_store_rebuild_selected_bases(store);
	if (scan->memory_chunk_valid && scan->memory_chunk < store->nchunks)
	{
		chunk = store->chunks[scan->memory_chunk];
		if (logical_row >= chunk->selected_base &&
			logical_row < chunk->selected_base + chunk->selected_count)
		{
			*chunk_index = scan->memory_chunk;
			*physical_row = chunk->selection[logical_row - chunk->selected_base];
			return chunk;
		}
	}
	while (low < high)
	{
		uint32 middle = low + (high - low) / 2;

		chunk = store->chunks[middle];
		if (logical_row < chunk->selected_base)
			high = middle;
		else if (logical_row >= chunk->selected_base + chunk->selected_count)
			low = middle + 1;
		else
		{
			scan->memory_chunk = middle;
			scan->memory_chunk_valid = true;
			*chunk_index = middle;
			*physical_row =
				chunk->selection[logical_row - chunk->selected_base];
			return chunk;
		}
	}
	elog(ERROR, "bloompg could not resolve selected vector row");
	pg_unreachable();
}

BloomPGColumnStore *
bloompg_column_store_create(MemoryContext parent,
	ResourceOwner runtime_owner, Size memory_limit)
{
	BloomPGColumnStore *store;
	MemoryContext context;
	MemoryContext old_context;

	Assert(parent != NULL);
	Assert(runtime_owner != NULL);
	context = AllocSetContextCreate(parent, "BloomPG vector materialization",
		ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(context);
	store = palloc0(sizeof(BloomPGColumnStore));
	store->memory_context = context;
	store->column_context = AllocSetContextCreate(context,
		"BloomPG vector chunks", ALLOCSET_DEFAULT_SIZES);
	store->runtime_owner = runtime_owner;
	store->memory_limit = Max(memory_limit, (Size) 64 * 1024);
	MemoryContextSwitchTo(old_context);
	return store;
}

void
bloompg_column_store_set_descriptor(BloomPGColumnStore *store,
	TupleDesc descriptor)
{
	Assert(store != NULL);
	Assert(descriptor != NULL);
	if (store->descriptor != NULL)
	{
		if (store->descriptor != descriptor)
			elog(ERROR, "bloompg materialization descriptor changed");
		return;
	}
	if (descriptor->natts <= 0)
		elog(ERROR, "bloompg materialization has no vectors");
	store->descriptor = descriptor;
	store->natts = descriptor->natts;
}

void
bloompg_column_store_append_slot(BloomPGColumnStore *store,
	TupleTableSlot *slot)
{
	BloomPGColumnChunk *chunk;
	uint32		row;
	MemoryContext old_context;
	int			attribute;

	Assert(store != NULL);
	Assert(store->descriptor != NULL);
	if (store->spill != NULL)
	{
		tuplestore_puttupleslot(store->spill, slot);
		bloompg_column_store_spill_set_active(store, store->rows);
		store->rows++;
		store->live_rows++;
		return;
	}
	if (store->nchunks == 0 ||
		store->chunks[store->nchunks - 1]->count == BLOOMPG_VECTOR_SIZE)
		chunk = bloompg_column_store_new_chunk(store);
	else
		chunk = store->chunks[store->nchunks - 1];
	row = chunk->count;
	slot_getallattrs(slot);
	old_context = MemoryContextSwitchTo(store->column_context);
	for (attribute = 0; attribute < store->natts; attribute++)
		bloompg_column_vector_append(store, &chunk->vectors[attribute], row,
			slot->tts_values[attribute], slot->tts_isnull[attribute]);
	MemoryContextSwitchTo(old_context);
	chunk->selection[chunk->selected_count++] = (uint16) row;
	chunk->count++;
	store->rows++;
	store->live_rows++;
	store->selection_bases_dirty = true;
	if (store->memory_bytes > store->memory_limit)
		bloompg_column_store_migrate_to_spill(store);
}

void
bloompg_column_store_begin_scan(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan, int eflags)
{
	MemSet(scan, 0, sizeof(*scan));
	scan->read_pointer = -1;
	if (store->spill != NULL)
	{
		scan->read_pointer = tuplestore_alloc_read_pointer(store->spill,
			EXEC_FLAG_REWIND |
			(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));
		scan->spill_slot = MakeSingleTupleTableSlot(store->descriptor,
			&TTSOpsMinimalTuple);
		tuplestore_select_read_pointer(store->spill, scan->read_pointer);
		tuplestore_rescan(store->spill);
	}
	else
		bloompg_column_store_rebuild_selected_bases(store);
}

void
bloompg_column_store_rescan(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan)
{
	scan->position = 0;
	scan->memory_chunk_valid = false;
	if (store->spill != NULL)
	{
		tuplestore_select_read_pointer(store->spill, scan->read_pointer);
		tuplestore_rescan(store->spill);
	}
}

bool
bloompg_column_store_next(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan, bool forward, TupleTableSlot *slot,
	uint64 *row_index)
{
	if (store->spill == NULL)
	{
		uint64		logical_row;
		uint32		chunk_index;
		uint32		physical_row;

		if (forward)
		{
			if (scan->position >= store->live_rows)
				return false;
			logical_row = scan->position++;
		}
		else
		{
			if (scan->position == 0)
				return false;
			logical_row = --scan->position;
		}
		(void) bloompg_column_store_find_selected(store, scan, logical_row,
			&chunk_index, &physical_row);
		bloompg_column_store_fill_memory_slot(store, chunk_index, physical_row,
			slot);
		if (row_index != NULL)
			*row_index = (uint64) chunk_index * BLOOMPG_VECTOR_SIZE +
				physical_row;
		return true;
	}
	for (;;)
	{
		uint64		index;

		if (forward)
		{
			if (scan->position >= store->rows)
				return false;
			index = scan->position++;
		}
		else
		{
			if (scan->position == 0)
				return false;
			index = --scan->position;
		}
		tuplestore_select_read_pointer(store->spill, scan->read_pointer);
		if (!tuplestore_gettupleslot(store->spill, forward, false,
			scan->spill_slot))
			return false;
		if (!bloompg_column_store_spill_is_active(store, index))
		{
			ExecClearTuple(scan->spill_slot);
			continue;
		}
		ExecCopySlot(slot, scan->spill_slot);
		ExecClearTuple(scan->spill_slot);
		if (row_index != NULL)
			*row_index = index;
		return true;
	}
}

uint32
bloompg_column_store_chunk_count(const BloomPGColumnStore *store)
{
	return store->spill == NULL ? store->nchunks : 0;
}

bool
bloompg_column_store_chunk_view(const BloomPGColumnStore *store,
	uint32 chunk_index, BloomPGColumnStoreChunkView *view)
{
	BloomPGColumnChunk *chunk;

	Assert(store != NULL);
	Assert(view != NULL);
	if (store->spill != NULL || chunk_index >= store->nchunks)
		return false;
	chunk = store->chunks[chunk_index];
	view->row_base = (uint64) chunk_index * BLOOMPG_VECTOR_SIZE;
	view->count = chunk->count;
	view->selected_count = chunk->selected_count;
	view->vector_count = store->natts;
	view->selection = chunk->selection;
	view->vectors = chunk->vectors;
	return true;
}

void
bloompg_column_store_set_chunk_selection(BloomPGColumnStore *store,
	uint32 chunk_index, const uint16 *selection, uint32 selected_count)
{
	BloomPGColumnChunk *chunk;
	uint32		old_count;
	uint32		index;

	if (store->spill != NULL || chunk_index >= store->nchunks)
		elog(ERROR, "invalid bloompg vector selection target");
	chunk = store->chunks[chunk_index];
	old_count = chunk->selected_count;
	if (selected_count > old_count)
		elog(ERROR, "bloompg vector selection grew during filtering");
	for (index = 0; index < selected_count; index++)
	{
		if (selection[index] >= chunk->count ||
			(index > 0 && selection[index] <= selection[index - 1]))
			elog(ERROR, "invalid bloompg vector selection");
	}
	if (selected_count > 0)
		memcpy(chunk->selection, selection,
			mul_size(selected_count, sizeof(uint16)));
	chunk->selected_count = (uint16) selected_count;
	store->live_rows -= old_count - selected_count;
	store->selection_rewrites++;
	store->selection_rows_examined += old_count;
	store->selection_rows_rejected += old_count - selected_count;
	store->selection_bases_dirty = true;
}

void
bloompg_column_store_end_scan(BloomPGColumnStoreScan *scan)
{
	if (scan->spill_slot != NULL)
	{
		ExecDropSingleTupleTableSlot(scan->spill_slot);
		scan->spill_slot = NULL;
	}
}

void
bloompg_column_store_deactivate(BloomPGColumnStore *store, uint64 row_index)
{
	if (row_index >= store->rows)
		elog(ERROR, "bloompg materialization row index is out of range");
	if (store->spill != NULL)
	{
		uint64 chunk_index = row_index / BLOOMPG_VECTOR_SIZE;
		uint32 row = (uint32) (row_index % BLOOMPG_VECTOR_SIZE);
		uint64 mask = UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS);
		uint64 *word;

		if (chunk_index >= store->spill_mask_count)
			return;
		word = &store->spill_masks[chunk_index]
			[row / BLOOMPG_VALIDITY_BITS];
		if ((*word & mask) != 0)
		{
			*word &= ~mask;
			store->live_rows--;
			store->selection_rows_rejected++;
		}
		return;
	}
	{
		uint64 chunk64 = row_index / BLOOMPG_VECTOR_SIZE;
		uint16 row = (uint16) (row_index % BLOOMPG_VECTOR_SIZE);
		BloomPGColumnChunk *chunk;
		uint32 index;

		if (chunk64 >= store->nchunks)
			return;
		chunk = store->chunks[chunk64];
		for (index = 0; index < chunk->selected_count; index++)
		{
			if (chunk->selection[index] != row)
				continue;
			if (index + 1 < chunk->selected_count)
				memmove(&chunk->selection[index], &chunk->selection[index + 1],
					(chunk->selected_count - index - 1) * sizeof(uint16));
			chunk->selected_count--;
			store->live_rows--;
			store->selection_rewrites++;
			store->selection_rows_examined++;
			store->selection_rows_rejected++;
			store->selection_bases_dirty = true;
			return;
		}
	}
}

void
bloompg_column_store_deactivate_all(BloomPGColumnStore *store)
{
	if (store->spill != NULL)
	{
		uint32 chunk_index;

		for (chunk_index = 0; chunk_index < store->spill_mask_count;
			 chunk_index++)
			MemSet(store->spill_masks[chunk_index], 0,
				sizeof(uint64) * BLOOMPG_VALIDITY_WORDS);
	}
	else
	{
		uint32 chunk_index;

		for (chunk_index = 0; chunk_index < store->nchunks; chunk_index++)
			store->chunks[chunk_index]->selected_count = 0;
		store->selection_bases_dirty = true;
	}
	store->selection_rewrites++;
	store->selection_rows_examined += store->live_rows;
	store->selection_rows_rejected += store->live_rows;
	store->live_rows = 0;
}

uint64
bloompg_column_store_rows(const BloomPGColumnStore *store)
{
	return store->rows;
}

uint64
bloompg_column_store_live_rows(const BloomPGColumnStore *store)
{
	return store->live_rows;
}

bool
bloompg_column_store_spilled(const BloomPGColumnStore *store)
{
	return store->spill != NULL;
}

Size
bloompg_column_store_memory_bytes(const BloomPGColumnStore *store)
{
	return store->memory_bytes;
}

Size
bloompg_column_store_peak_memory_bytes(const BloomPGColumnStore *store)
{
	return store->peak_memory_bytes;
}

Size
bloompg_column_store_memory_limit(const BloomPGColumnStore *store)
{
	return store->memory_limit;
}

uint64
bloompg_column_store_spill_rows(const BloomPGColumnStore *store)
{
	return store->spill_rows;
}

uint32
bloompg_column_store_vector_chunks(const BloomPGColumnStore *store)
{
	return store->peak_chunks;
}

uint64
bloompg_column_store_varlen_values(const BloomPGColumnStore *store)
{
	return store->varlen_values;
}

Size
bloompg_column_store_varlen_bytes(const BloomPGColumnStore *store)
{
	return store->varlen_bytes;
}

uint64
bloompg_column_store_selection_rewrites(const BloomPGColumnStore *store)
{
	return store->selection_rewrites;
}

uint64
bloompg_column_store_selection_rows_examined(const BloomPGColumnStore *store)
{
	return store->selection_rows_examined;
}

uint64
bloompg_column_store_selection_rows_rejected(const BloomPGColumnStore *store)
{
	return store->selection_rows_rejected;
}

uint64
bloompg_column_store_slot_reconstructions(const BloomPGColumnStore *store)
{
	return store->slot_reconstructions;
}

void
bloompg_column_store_destroy(BloomPGColumnStore *store)
{
	MemoryContext context;

	if (store == NULL)
		return;
	context = store->memory_context;
	if (store->spill != NULL)
	{
		tuplestore_end(store->spill);
		store->spill = NULL;
	}
	MemoryContextDelete(context);
}
