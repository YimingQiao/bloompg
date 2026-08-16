#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/scan.h"
#include "bloompg/filter.h"
#include "bloompg/runtime.h"

#include "access/tableam.h"
#include "access/htup_details.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "port/atomics.h"
#include "utils/snapmgr.h"
#include "bloompg/scan_internal.h"

/* DSM serialization and lock-free parallel materialization cursors. */

static Size
bloompg_parallel_store_header_size(uint64 items)
{
	Size offsets_size;

	if (items > (uint64) (MaxAllocSize / sizeof(uint64)) - 1)
		elog(ERROR, "bloompg parallel materialization has too many rows");
	offsets_size = mul_size((Size) items + 1, sizeof(uint64));
	return MAXALIGN(add_size(offsetof(BloomPGParallelStore, offsets),
							   offsets_size));
}

static Size
bloompg_parallel_vector_width(BloomPGVectorPhysicalType physical_type)
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

static Size
bloompg_parallel_column_chunk_header_size(uint32 vector_count)
{
	return MAXALIGN(add_size(offsetof(BloomPGParallelColumnChunk, vectors),
		mul_size(vector_count, sizeof(BloomPGParallelColumnVector))));
}

static Size
bloompg_parallel_column_chunk_size(const BloomPGColumnStoreChunkView *view)
{
	Size size = bloompg_parallel_column_chunk_header_size(view->vector_count);
	uint32 validity_words = (view->count + BLOOMPG_VALIDITY_BITS - 1) /
		BLOOMPG_VALIDITY_BITS;
	uint32 attribute;

	size = add_size(size, MAXALIGN(mul_size(view->selected_count,
		sizeof(uint16))));
	for (attribute = 0; attribute < view->vector_count; attribute++)
	{
		const BloomPGColumnVector *vector = &view->vectors[attribute];

		size = add_size(size, MAXALIGN(mul_size(view->count,
			bloompg_parallel_vector_width(vector->physical_type))));
		size = add_size(size, MAXALIGN(mul_size(validity_words,
			sizeof(uint64))));
		if (vector->physical_type == BLOOMPG_VECTOR_BYREF)
			size = add_size(size, MAXALIGN(vector->heap_used));
	}
	return size;
}

static Size
bloompg_estimate_parallel_column_store(BloomPGRuntimeMaterializationEntry *entry)
{
	uint32 chunk_count = bloompg_column_store_chunk_count(entry->store);
	Size size = bloompg_parallel_store_header_size(chunk_count);
	uint64 rows = 0;
	uint32 chunk_index;

	for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
	{
		BloomPGColumnStoreChunkView view;

		if (!bloompg_column_store_chunk_view(entry->store, chunk_index, &view))
			elog(ERROR, "bloompg lost column chunk during parallel estimate");
		size = add_size(size, bloompg_parallel_column_chunk_size(&view));
		rows += view.selected_count;
	}
	if (rows != entry->rows)
		elog(ERROR, "bloompg materialization row count changed before parallel execution");
	return size;
}

static Size
bloompg_estimate_parallel_tuple_store(BloomPGRuntimeMaterializationEntry *entry)
{
	TupleTableSlot *slot;
	BloomPGColumnStoreScan scan;
	Size data_bytes = 0;
	uint64 rows = 0;

	if (entry == NULL || entry->store == NULL || entry->descriptor == NULL)
		return bloompg_parallel_store_header_size(0);
	slot = MakeSingleTupleTableSlot(entry->descriptor, &TTSOpsVirtual);
	bloompg_column_store_begin_scan(entry->store, &scan, EXEC_FLAG_REWIND);
	while (bloompg_column_store_next(entry->store, &scan, true, slot, NULL))
	{
		bool should_free;
		MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &should_free);

		data_bytes = add_size(data_bytes, MAXALIGN(tuple->t_len));
		rows++;
		if (should_free)
			heap_free_minimal_tuple(tuple);
		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);
	bloompg_column_store_end_scan(&scan);
	if (rows != entry->rows)
		elog(ERROR, "bloompg materialization row count changed before parallel execution");
	return add_size(bloompg_parallel_store_header_size(rows), data_bytes);
}

Size
bloompg_estimate_parallel_store(BloomPGRuntimeMaterializationEntry *entry)
{
	if (entry == NULL || entry->store == NULL || entry->descriptor == NULL)
		return bloompg_parallel_store_header_size(0);
	if (!bloompg_column_store_spilled(entry->store))
		return bloompg_estimate_parallel_column_store(entry);
	return bloompg_estimate_parallel_tuple_store(entry);
}

static void
bloompg_fill_parallel_column_store(BloomPGRuntimeMaterializationEntry *entry,
	BloomPGParallelStore *parallel_store, Size allocated_size)
{
	char *base = (char *) parallel_store;
	uint32 chunk_count = bloompg_column_store_chunk_count(entry->store);
	Size used = bloompg_parallel_store_header_size(chunk_count);
	uint64 rows = 0;
	uint32 chunk_index;

	parallel_store->format = BLOOMPG_PARALLEL_STORE_COLUMNS;
	parallel_store->rows = entry->rows;
	parallel_store->chunks = chunk_count;
	parallel_store->natts = entry->descriptor->natts;
	parallel_store->data_offset = used;
	for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
	{
		BloomPGColumnStoreChunkView view;
		BloomPGParallelColumnChunk *target;
		Size chunk_size;
		Size header_size;
		uint32 validity_words;
		uint32 attribute;

		if (!bloompg_column_store_chunk_view(entry->store, chunk_index, &view))
			elog(ERROR, "bloompg lost column chunk during parallel copy");
		chunk_size = bloompg_parallel_column_chunk_size(&view);
		if (used > allocated_size || chunk_size > allocated_size - used)
			elog(ERROR, "bloompg parallel column store exceeded its DSM estimate");
		parallel_store->offsets[chunk_index] = used;
		target = (BloomPGParallelColumnChunk *) (base + used);
		header_size = bloompg_parallel_column_chunk_header_size(view.vector_count);
		target->count = view.count;
		target->selected_count = view.selected_count;
		target->vector_count = view.vector_count;
		used += header_size;
		target->selection_offset = used;
		if (view.selected_count > 0)
			memcpy(base + used, view.selection,
				mul_size(view.selected_count, sizeof(uint16)));
		used += MAXALIGN(mul_size(view.selected_count, sizeof(uint16)));
		validity_words = (view.count + BLOOMPG_VALIDITY_BITS - 1) /
			BLOOMPG_VALIDITY_BITS;
		for (attribute = 0; attribute < view.vector_count; attribute++)
		{
			const BloomPGColumnVector *source = &view.vectors[attribute];
			BloomPGParallelColumnVector *vector = &target->vectors[attribute];
			Size data_bytes = mul_size(view.count,
				bloompg_parallel_vector_width(source->physical_type));
			Size validity_bytes = mul_size(validity_words, sizeof(uint64));

			vector->type_oid = source->type_oid;
			vector->typlen = source->typlen;
			vector->physical_type = (uint8) source->physical_type;
			vector->heap_used = source->heap_used;
			vector->data_offset = used;
			if (data_bytes > 0)
				memcpy(base + used, source->data, data_bytes);
			used += MAXALIGN(data_bytes);
			vector->validity_offset = used;
			if (validity_bytes > 0)
				memcpy(base + used, source->validity, validity_bytes);
			used += MAXALIGN(validity_bytes);
			if (source->physical_type == BLOOMPG_VECTOR_BYREF)
			{
				vector->heap_offset = used;
				if (source->heap_used > 0)
					memcpy(base + used, source->heap, source->heap_used);
				used += MAXALIGN(source->heap_used);
			}
		}
		if (used != parallel_store->offsets[chunk_index] + chunk_size)
			elog(ERROR, "bloompg parallel column chunk size changed during copy");
		rows += view.selected_count;
	}
	if (rows != entry->rows || used > allocated_size)
		elog(ERROR, "bloompg materialization row count changed during parallel setup");
	parallel_store->offsets[chunk_count] = used;
	parallel_store->data_bytes = used - parallel_store->data_offset;
}

static void
bloompg_fill_parallel_tuple_store(BloomPGRuntimeMaterializationEntry *entry,
	BloomPGParallelStore *parallel_store, Size allocated_size)
{
	TupleTableSlot *slot;
	BloomPGColumnStoreScan scan;
	char *data;
	Size used = 0;
	uint64 rows = 0;

	parallel_store->format = BLOOMPG_PARALLEL_STORE_TUPLES;
	parallel_store->rows = entry->rows;
	parallel_store->data_offset = bloompg_parallel_store_header_size(entry->rows);
	if (parallel_store->data_offset > allocated_size)
		elog(ERROR, "bloompg parallel materialization metadata overflow");
	data = (char *) parallel_store + parallel_store->data_offset;
	slot = MakeSingleTupleTableSlot(entry->descriptor, &TTSOpsVirtual);
	bloompg_column_store_begin_scan(entry->store, &scan, EXEC_FLAG_REWIND);
	while (bloompg_column_store_next(entry->store, &scan, true, slot, NULL))
	{
		bool should_free;
		MinimalTuple tuple = ExecFetchSlotMinimalTuple(slot, &should_free);
		Size tuple_space = MAXALIGN(tuple->t_len);

		if (rows >= entry->rows ||
			parallel_store->data_offset + used > allocated_size ||
			tuple_space > allocated_size - parallel_store->data_offset - used)
			elog(ERROR, "bloompg parallel materialization exceeded its DSM estimate");
		parallel_store->offsets[rows] = used;
		memcpy(data + used, tuple, tuple->t_len);
		used += tuple_space;
		rows++;
		if (should_free)
			heap_free_minimal_tuple(tuple);
		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);
	bloompg_column_store_end_scan(&scan);
	if (rows != entry->rows)
		elog(ERROR, "bloompg materialization row count changed during parallel setup");
	parallel_store->offsets[rows] = used;
	parallel_store->data_bytes = used;
}

void
bloompg_fill_parallel_store(BloomPGRuntimeMaterializationEntry *entry,
	BloomPGParallelStore *parallel_store, Size allocated_size)
{
	MemSet(parallel_store, 0, allocated_size);
	pg_atomic_init_u64(&parallel_store->next_item, 0);
	parallel_store->total_size = allocated_size;
	if (entry == NULL || entry->store == NULL || entry->descriptor == NULL)
		return;
	if (!bloompg_column_store_spilled(entry->store))
		bloompg_fill_parallel_column_store(entry, parallel_store, allocated_size);
	else
		bloompg_fill_parallel_tuple_store(entry, parallel_store, allocated_size);
	parallel_store->available = true;
}

MinimalTuple
bloompg_parallel_next_tuple(BloomPGParallelStore *parallel_store)
{
	uint64 index;
	uint64 offset;
	uint64 next_offset;
	char *data;

	if (parallel_store == NULL || !parallel_store->available ||
		parallel_store->format != BLOOMPG_PARALLEL_STORE_TUPLES)
		return NULL;
	index = pg_atomic_fetch_add_u64(&parallel_store->next_item, 1);
	if (index >= parallel_store->rows)
		return NULL;
	offset = parallel_store->offsets[index];
	next_offset = parallel_store->offsets[index + 1];
	if (offset >= next_offset || next_offset > parallel_store->data_bytes)
		elog(ERROR, "invalid bloompg parallel materialization offset");
	data = (char *) parallel_store + parallel_store->data_offset;
	return (MinimalTuple) (data + offset);
}

static bool
bloompg_parallel_range_valid(const BloomPGParallelStore *store,
	Size offset, Size bytes)
{
	return offset <= store->total_size && bytes <= store->total_size - offset;
}

static BloomPGParallelColumnChunk *
bloompg_parallel_column_chunk(BloomPGParallelStore *store, uint64 chunk_index)
{
	uint64 offset;
	uint64 next_offset;
	Size header_size;
	BloomPGParallelColumnChunk *chunk;

	if (chunk_index >= store->chunks)
		return NULL;
	offset = store->offsets[chunk_index];
	next_offset = store->offsets[chunk_index + 1];
	if (offset >= next_offset || next_offset > store->total_size)
		elog(ERROR, "invalid bloompg parallel column chunk offset");
	chunk = (BloomPGParallelColumnChunk *) ((char *) store + offset);
	if (chunk->vector_count != store->natts)
		elog(ERROR, "invalid bloompg parallel column count");
	header_size = bloompg_parallel_column_chunk_header_size(chunk->vector_count);
	if (header_size > next_offset - offset)
		elog(ERROR, "invalid bloompg parallel column metadata");
	return chunk;
}

bool
bloompg_parallel_next_column(BloomPGParallelStore *store,
	BloomPGParallelCursor *cursor, TupleTableSlot *slot)
{
	char *base = (char *) store;
	BloomPGParallelColumnChunk *chunk;
	const uint16 *selection;
	uint32 row;
	uint32 attribute;

	if (store == NULL || !store->available ||
		store->format != BLOOMPG_PARALLEL_STORE_COLUMNS)
		return false;
	for (;;)
	{
		if (!cursor->chunk_valid)
		{
			cursor->chunk_index = pg_atomic_fetch_add_u64(&store->next_item, 1);
			if (cursor->chunk_index >= store->chunks)
				return false;
			cursor->selected_index = 0;
			cursor->chunk_valid = true;
		}
		chunk = bloompg_parallel_column_chunk(store, cursor->chunk_index);
		if (cursor->selected_index < chunk->selected_count)
			break;
		cursor->chunk_valid = false;
	}
	if (!bloompg_parallel_range_valid(store, chunk->selection_offset,
		mul_size(chunk->selected_count, sizeof(uint16))))
		elog(ERROR, "invalid bloompg parallel selection vector");
	selection = (const uint16 *) (base + chunk->selection_offset);
	row = selection[cursor->selected_index++];
	if (row >= chunk->count)
		elog(ERROR, "invalid bloompg parallel selected row");
	if ((uint32) slot->tts_tupleDescriptor->natts != store->natts)
		elog(ERROR, "bloompg parallel slot descriptor changed");

	ExecClearTuple(slot);
	for (attribute = 0; attribute < store->natts; attribute++)
	{
		BloomPGParallelColumnVector *vector = &chunk->vectors[attribute];
		BloomPGVectorPhysicalType physical_type =
			(BloomPGVectorPhysicalType) vector->physical_type;
		Size width = bloompg_parallel_vector_width(physical_type);
		uint32 validity_words = (chunk->count + BLOOMPG_VALIDITY_BITS - 1) /
			BLOOMPG_VALIDITY_BITS;
		const uint64 *validity;
		const char *data;

		if (!bloompg_parallel_range_valid(store, vector->data_offset,
			mul_size(chunk->count, width)) ||
			!bloompg_parallel_range_valid(store, vector->validity_offset,
				mul_size(validity_words, sizeof(uint64))))
			elog(ERROR, "invalid bloompg parallel column vector");
		validity = (const uint64 *) (base + vector->validity_offset);
		slot->tts_isnull[attribute] =
			(validity[row / BLOOMPG_VALIDITY_BITS] &
				(UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS))) == 0;
		if (slot->tts_isnull[attribute])
		{
			slot->tts_values[attribute] = (Datum) 0;
			continue;
		}
		data = base + vector->data_offset;
		switch (physical_type)
		{
			case BLOOMPG_VECTOR_BYVAL_1:
				slot->tts_values[attribute] = (Datum) ((const uint8 *) data)[row];
				break;
			case BLOOMPG_VECTOR_BYVAL_2:
				slot->tts_values[attribute] = (Datum) ((const uint16 *) data)[row];
				break;
			case BLOOMPG_VECTOR_BYVAL_4:
				slot->tts_values[attribute] = (Datum) ((const uint32 *) data)[row];
				break;
			case BLOOMPG_VECTOR_BYVAL_8:
				slot->tts_values[attribute] = (Datum) ((const uint64 *) data)[row];
				break;
			case BLOOMPG_VECTOR_BYREF:
			{
				uint32 heap_offset = ((const uint32 *) data)[row];

				if (heap_offset >= vector->heap_used ||
					!bloompg_parallel_range_valid(store, vector->heap_offset,
						vector->heap_used))
					elog(ERROR, "invalid bloompg parallel by-reference datum");
				slot->tts_values[attribute] = PointerGetDatum(
					base + vector->heap_offset + heap_offset);
				break;
			}
		}
	}
	ExecStoreVirtualTuple(slot);
	return true;
}
