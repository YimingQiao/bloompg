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

Size
bloompg_estimate_parallel_store(BloomPGRuntimeMaterializationEntry *entry)
{
	if (entry == NULL || entry->store == NULL || entry->descriptor == NULL)
		return bloompg_parallel_store_header_size(0);
	return bloompg_estimate_parallel_column_store(entry);
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

void
bloompg_fill_parallel_store(BloomPGRuntimeMaterializationEntry *entry,
	BloomPGParallelStore *parallel_store, Size allocated_size)
{
	MemSet(parallel_store, 0, allocated_size);
	pg_atomic_init_u64(&parallel_store->next_item, 0);
	parallel_store->total_size = allocated_size;
	if (entry == NULL || entry->store == NULL || entry->descriptor == NULL)
		return;
	bloompg_fill_parallel_column_store(entry, parallel_store, allocated_size);
	parallel_store->available = true;
}

static bool
bloompg_parallel_range_valid(Size lower, Size upper, Size offset, Size bytes)
{
	return offset >= lower && offset <= upper && bytes <= upper - offset;
}

static BloomPGParallelColumnChunk *
bloompg_parallel_validate_column_chunk(BloomPGParallelStore *store,
	uint64 chunk_index, TupleDesc descriptor)
{
	char *base = (char *) store;
	uint64 offset;
	uint64 next_offset;
	Size header_size;
	Size expected;
	BloomPGParallelColumnChunk *chunk;
	const uint16 *selection;
	uint32 attribute;
	uint32 selected;

	if (store->format != BLOOMPG_PARALLEL_STORE_COLUMNS ||
		store->natts != (uint32) descriptor->natts || store->natts == 0)
		elog(ERROR, "invalid bloompg parallel store descriptor");
	header_size = bloompg_parallel_store_header_size(store->chunks);
	if (header_size > store->total_size || store->data_offset != header_size ||
		store->data_bytes != store->total_size - store->data_offset ||
		store->offsets[store->chunks] != store->total_size ||
		chunk_index >= store->chunks)
		elog(ERROR, "invalid bloompg parallel store header");
	offset = store->offsets[chunk_index];
	next_offset = store->offsets[chunk_index + 1];
	if (offset < store->data_offset || offset >= next_offset ||
		next_offset > store->total_size ||
		next_offset - offset < offsetof(BloomPGParallelColumnChunk, vectors))
		elog(ERROR, "invalid bloompg parallel column chunk offset");
	chunk = (BloomPGParallelColumnChunk *) (base + offset);
	if (chunk->vector_count != store->natts ||
		chunk->count > BLOOMPG_VECTOR_SIZE ||
		chunk->selected_count > chunk->count)
		elog(ERROR, "invalid bloompg parallel column count");
	header_size = bloompg_parallel_column_chunk_header_size(chunk->vector_count);
	if (header_size > next_offset - offset)
		elog(ERROR, "invalid bloompg parallel column metadata");
	expected = add_size((Size) offset, header_size);
	if (chunk->selection_offset != expected ||
		!bloompg_parallel_range_valid(offset, next_offset,
			chunk->selection_offset,
			mul_size(chunk->selected_count, sizeof(uint16))))
		elog(ERROR, "invalid bloompg parallel selection vector");
	selection = (const uint16 *) (base + chunk->selection_offset);
	for (selected = 0; selected < chunk->selected_count; selected++)
	{
		if (selection[selected] >= chunk->count ||
			(selected > 0 && selection[selected] <= selection[selected - 1]))
			elog(ERROR, "invalid bloompg parallel selected row");
	}
	expected = add_size(expected, MAXALIGN(mul_size(chunk->selected_count,
		sizeof(uint16))));
	for (attribute = 0; attribute < store->natts; attribute++)
	{
		BloomPGParallelColumnVector *vector = &chunk->vectors[attribute];
		Form_pg_attribute form = TupleDescAttr(descriptor, attribute);
		BloomPGVectorPhysicalType physical_type;
		Size data_bytes;
		Size validity_bytes;
		uint32 validity_words =
			(chunk->count + BLOOMPG_VALIDITY_BITS - 1) /
			BLOOMPG_VALIDITY_BITS;

		if (!form->attbyval)
			physical_type = BLOOMPG_VECTOR_BYREF;
		else if (form->attlen == 1)
			physical_type = BLOOMPG_VECTOR_BYVAL_1;
		else if (form->attlen == 2)
			physical_type = BLOOMPG_VECTOR_BYVAL_2;
		else if (form->attlen == 4)
			physical_type = BLOOMPG_VECTOR_BYVAL_4;
		else if (form->attlen == 8)
			physical_type = BLOOMPG_VECTOR_BYVAL_8;
		else
			elog(ERROR, "invalid bloompg parallel by-value width");
		if (vector->type_oid != form->atttypid ||
			vector->typlen != form->attlen ||
			vector->physical_type != (uint8) physical_type)
			elog(ERROR, "invalid bloompg parallel vector descriptor");
		data_bytes = mul_size(chunk->count,
			bloompg_parallel_vector_width(physical_type));
		validity_bytes = mul_size(validity_words, sizeof(uint64));
		if (vector->data_offset != expected ||
			!bloompg_parallel_range_valid(offset, next_offset,
				vector->data_offset, data_bytes))
			elog(ERROR, "invalid bloompg parallel vector data");
		expected = add_size(expected, MAXALIGN(data_bytes));
		if (vector->validity_offset != expected ||
			!bloompg_parallel_range_valid(offset, next_offset,
				vector->validity_offset, validity_bytes))
			elog(ERROR, "invalid bloompg parallel vector validity");
		expected = add_size(expected, MAXALIGN(validity_bytes));
		if (physical_type == BLOOMPG_VECTOR_BYREF)
		{
			const uint64 *validity =
				(const uint64 *) (base + vector->validity_offset);
			const uint32 *offsets =
				(const uint32 *) (base + vector->data_offset);
			uint32 row;

			if (vector->heap_offset != expected ||
				!bloompg_parallel_range_valid(offset, next_offset,
					vector->heap_offset, vector->heap_used))
				elog(ERROR, "invalid bloompg parallel vector heap");
			for (row = 0; row < chunk->count; row++)
			{
				Size datum_size;

				if ((validity[row / BLOOMPG_VALIDITY_BITS] &
						(UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS))) == 0)
					continue;
				if ((offsets[row] % MAXIMUM_ALIGNOF) != 0 ||
					!bloompg_byref_datum_size(base + vector->heap_offset,
						vector->heap_used, vector->typlen, offsets[row],
						&datum_size))
					elog(ERROR, "invalid bloompg parallel by-reference datum");
			}
			expected = add_size(expected, MAXALIGN(vector->heap_used));
		}
		else if (vector->heap_offset != 0 || vector->heap_used != 0)
			elog(ERROR, "invalid bloompg fixed-width parallel vector heap");
	}
	if (expected != next_offset)
		elog(ERROR, "invalid bloompg parallel column chunk length");
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
			chunk = bloompg_parallel_validate_column_chunk(store,
				cursor->chunk_index, slot->tts_tupleDescriptor);
			cursor->chunk_valid = true;
		}
		else
			chunk = (BloomPGParallelColumnChunk *) (base +
				store->offsets[cursor->chunk_index]);
		if (cursor->selected_index < chunk->selected_count)
			break;
		cursor->chunk_valid = false;
	}
	selection = (const uint16 *) (base + chunk->selection_offset);
	row = selection[cursor->selected_index++];

	ExecClearTuple(slot);
	for (attribute = 0; attribute < store->natts; attribute++)
	{
		BloomPGParallelColumnVector *vector = &chunk->vectors[attribute];
		BloomPGVectorPhysicalType physical_type =
			(BloomPGVectorPhysicalType) vector->physical_type;
		const uint64 *validity;
		const char *data;

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

				slot->tts_values[attribute] = PointerGetDatum(
					base + vector->heap_offset + heap_offset);
				break;
			}
		}
	}
	ExecStoreVirtualTuple(slot);
	return true;
}
