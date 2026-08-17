#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/scan.h"
#include "bloompg/filter.h"
#include "bloompg/graph.h"
#include "bloompg/runtime.h"
#include "bloompg/sampling.h"
#include "bloompg/transfer.h"
#include "bloompg/transfer_internal.h"

#include "catalog/pg_class.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_type_d.h"
#include "access/tableam.h"
#include "access/sysattr.h"
#include "executor/executor.h"
#include "executor/execdesc.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "tcop/dest.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/date.h"
#include "utils/typcache.h"

#include <float.h>


/* Physical scan execution, row-to-column materialization, and worker waves. */

/* Accepted materializations outlive the optimizer's internal subtransaction,
 * so both the store and its shared budget live in TopTransactionContext. */
BloomPGColumnStore *
bloompg_begin_runtime_store(BloomPGMemoryBudget *memory_budget)
{
	Assert(memory_budget != NULL);
	return bloompg_column_store_create(TopTransactionContext, memory_budget);
}

bool
bloompg_integral_value(Datum datum, Oid type_oid, int64 *result)
{
	switch (type_oid)
	{
		case INT2OID:
			*result = DatumGetInt16(datum);
			return true;
		case INT4OID:
			*result = DatumGetInt32(datum);
			return true;
		case INT8OID:
			*result = DatumGetInt64(datum);
			return true;
		case OIDOID:
			*result = (int64) DatumGetObjectId(datum);
			return true;
		case DATEOID:
			*result = (int64) DatumGetDateADT(datum);
			return true;
		default:
			return false;
	}
}

/* Bias the sign bit so ordinary unsigned ordering is also signed int64
 * ordering, then retain word-aligned ranges.  Growing by whole words keeps
 * expansion a memcpy instead of a bit shift. */
static bool
bloompg_pending_bitmap_add_slow(BloomPGPendingTransfer *pending,
	uint64 biased, uint64 wanted_word)
{
	const uint64 maximum_word = UINT64_MAX >> 6;
	const uint64 initial_words = 64;
	uint64 max_words = ((uint64) bloompg_max_filter_bytes *
		BITS_PER_BYTE) / 64;
	uint64 lower_word;
	uint64 upper_word;
	uint64 word_count;
	uint64 *words;

	Assert(pending->build_filter && !pending->bitmap_candidate_abandoned);
	if (pending->bitmap_candidate_words == NULL)
	{
		word_count = Min(initial_words, max_words);
		lower_word = wanted_word >= word_count / 2 ?
			wanted_word - word_count / 2 : 0;
		if (lower_word > maximum_word - (word_count - 1))
			lower_word = maximum_word - (word_count - 1);
		pending->bitmap_candidate_words = MemoryContextAllocZero(
			pending->filter->memory_context,
			mul_size((Size) word_count, sizeof(uint64)));
		pending->bitmap_candidate_lower_word = lower_word;
		pending->bitmap_candidate_upper_word =
			lower_word + word_count - 1;
	}
	else if (wanted_word < pending->bitmap_candidate_lower_word ||
		wanted_word > pending->bitmap_candidate_upper_word)
	{
		uint64 old_words = pending->bitmap_candidate_upper_word -
			pending->bitmap_candidate_lower_word + 1;
		uint64 needed_lower = Min(wanted_word,
			pending->bitmap_candidate_lower_word);
		uint64 needed_upper = Max(wanted_word,
			pending->bitmap_candidate_upper_word);
		uint64 needed_words = needed_upper - needed_lower + 1;

		word_count = old_words;
		while (word_count < needed_words && word_count < max_words)
			word_count = word_count > max_words / 2 ? max_words :
				word_count * 2;
		if (word_count == 0 || word_count > max_words ||
			word_count < needed_words ||
			word_count > MaxAllocSize / sizeof(uint64))
		{
			pfree(pending->bitmap_candidate_words);
			pending->bitmap_candidate_words = NULL;
			pending->bitmap_candidate_abandoned = true;
			return false;
		}
		if (wanted_word < pending->bitmap_candidate_lower_word)
		{
			upper_word = pending->bitmap_candidate_upper_word;
			lower_word = upper_word >= word_count - 1 ?
				upper_word - (word_count - 1) : 0;
		}
		else
		{
			lower_word = pending->bitmap_candidate_lower_word;
			if (lower_word > maximum_word - (word_count - 1))
				lower_word = maximum_word - (word_count - 1);
			upper_word = lower_word + word_count - 1;
		}
		if (wanted_word < lower_word || wanted_word > upper_word ||
			pending->bitmap_candidate_lower_word < lower_word ||
			pending->bitmap_candidate_upper_word > upper_word)
		{
			pfree(pending->bitmap_candidate_words);
			pending->bitmap_candidate_words = NULL;
			pending->bitmap_candidate_abandoned = true;
			return false;
		}
		words = MemoryContextAllocZero(pending->filter->memory_context,
			mul_size((Size) word_count, sizeof(uint64)));
		memcpy(words + (pending->bitmap_candidate_lower_word - lower_word),
			pending->bitmap_candidate_words,
			(Size) (pending->bitmap_candidate_upper_word -
				pending->bitmap_candidate_lower_word + 1) * sizeof(uint64));
		pfree(pending->bitmap_candidate_words);
		pending->bitmap_candidate_words = words;
		pending->bitmap_candidate_lower_word = lower_word;
		pending->bitmap_candidate_upper_word = upper_word;
	}
	pending->bitmap_candidate_words[wanted_word -
		pending->bitmap_candidate_lower_word] |=
		UINT64CONST(1) << (biased & 63);
	return true;
}

/* Once the candidate range has settled, bitmap construction is just one
 * bounds check and one word update.  Keep allocation/range growth out of the
 * per-value path: dense integer join keys can otherwise spend more time
 * entering the general helper than setting bits. */
static inline bool
bloompg_pending_bitmap_set(BloomPGPendingTransfer *pending, int64 value)
{
	const uint64 sign_bit = UINT64CONST(0x8000000000000000);
	uint64 biased;
	uint64 wanted_word;

	if (!pending->build_filter || pending->bitmap_candidate_abandoned)
		return false;
	biased = ((uint64) value) ^ sign_bit;
	wanted_word = biased >> 6;
	if (likely(pending->bitmap_candidate_words != NULL &&
		wanted_word >= pending->bitmap_candidate_lower_word &&
		wanted_word <= pending->bitmap_candidate_upper_word))
	{
		pending->bitmap_candidate_words[wanted_word -
			pending->bitmap_candidate_lower_word] |=
			UINT64CONST(1) << (biased & 63);
		return true;
	}
	return bloompg_pending_bitmap_add_slow(pending, biased, wanted_word);
}

static inline void
bloompg_pending_bitmap_add(BloomPGPendingTransfer *pending, int64 value)
{
	if (bloompg_pending_bitmap_set(pending, value))
		pending->bitmap_candidate_inserted++;
}

typedef struct BloomPGIntegralAccumulator
{
	bool		has_range;
	int64		minimum;
	int64		maximum;
	uint64		bitmap_inserted;
} BloomPGIntegralAccumulator;

static inline void
bloompg_pending_accumulate_integral(BloomPGPendingTransfer *pending,
	BloomPGIntegralAccumulator *accumulator, int64 value)
{
	if (!accumulator->has_range)
		accumulator->minimum = accumulator->maximum = value;
	else
	{
		accumulator->minimum = Min(accumulator->minimum, value);
		accumulator->maximum = Max(accumulator->maximum, value);
	}
	accumulator->has_range = true;
	if (bloompg_pending_bitmap_set(pending, value))
		accumulator->bitmap_inserted++;
}

static inline bool
bloompg_vector_row_valid(const BloomPGColumnVector *vector, uint32 row)
{
	return (vector->validity[row / BLOOMPG_VALIDITY_BITS] &
		(UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS))) != 0;
}

static void
bloompg_process_pending_integral_chunk(BloomPGPendingTransfer *pending,
	const BloomPGColumnStoreChunkView *view, const uint16 *selection,
	uint32 selected_count)
{
	AttrNumber attribute = pending->spec.build_attributes[0];
	const BloomPGColumnVector *vector;
	BloomPGIntegralAccumulator accumulator;
	uint32 selected;

	if (!AttributeNumberIsValid(attribute) ||
		(uint32) attribute > view->vector_count)
		elog(ERROR, "invalid bloompg column-store range attribute");
	vector = &view->vectors[attribute - 1];
	accumulator.has_range = pending->has_range;
	accumulator.minimum = pending->minimum;
	accumulator.maximum = pending->maximum;
	accumulator.bitmap_inserted = 0;
	switch (pending->filter->keys[0].build_type)
	{
		case INT2OID:
			if (vector->physical_type != BLOOMPG_VECTOR_BYVAL_2)
				elog(ERROR, "invalid bloompg int2 vector representation");
			for (selected = 0; selected < selected_count; selected++)
			{
				uint32 row = selection[selected];

				if (row >= view->count)
					elog(ERROR, "invalid bloompg vector selection");
				if (bloompg_vector_row_valid(vector, row))
					bloompg_pending_accumulate_integral(pending, &accumulator,
						(int64) (int16) ((const uint16 *) vector->data)[row]);
			}
			break;
		case INT4OID:
		case DATEOID:
			if (vector->physical_type != BLOOMPG_VECTOR_BYVAL_4)
				elog(ERROR, "invalid bloompg int4 vector representation");
			for (selected = 0; selected < selected_count; selected++)
			{
				uint32 row = selection[selected];

				if (row >= view->count)
					elog(ERROR, "invalid bloompg vector selection");
				if (bloompg_vector_row_valid(vector, row))
					bloompg_pending_accumulate_integral(pending, &accumulator,
						(int64) (int32) ((const uint32 *) vector->data)[row]);
			}
			break;
		case INT8OID:
			if (vector->physical_type != BLOOMPG_VECTOR_BYVAL_8)
				elog(ERROR, "invalid bloompg int8 vector representation");
			for (selected = 0; selected < selected_count; selected++)
			{
				uint32 row = selection[selected];

				if (row >= view->count)
					elog(ERROR, "invalid bloompg vector selection");
				if (bloompg_vector_row_valid(vector, row))
					bloompg_pending_accumulate_integral(pending, &accumulator,
						(int64) ((const uint64 *) vector->data)[row]);
			}
			break;
		case OIDOID:
			if (vector->physical_type != BLOOMPG_VECTOR_BYVAL_4)
				elog(ERROR, "invalid bloompg oid vector representation");
			for (selected = 0; selected < selected_count; selected++)
			{
				uint32 row = selection[selected];

				if (row >= view->count)
					elog(ERROR, "invalid bloompg vector selection");
				if (bloompg_vector_row_valid(vector, row))
					bloompg_pending_accumulate_integral(pending, &accumulator,
						(int64) ((const uint32 *) vector->data)[row]);
			}
			break;
		default:
			elog(ERROR, "invalid bloompg integral vector type %u",
				pending->filter->keys[0].build_type);
	}
	pending->has_range = accumulator.has_range;
	pending->minimum = accumulator.minimum;
	pending->maximum = accumulator.maximum;
	pending->bitmap_candidate_inserted += accumulator.bitmap_inserted;
}

/* Process a completed vector chunk filter-by-filter.  This hoists the PG
 * List walk, attribute lookup, and physical-type dispatch out of the row
 * loop while retaining one pass over each requested outgoing key. */
void
bloompg_process_pending_chunk(List *pending_transfers,
	const BloomPGColumnStoreChunkView *view, const uint16 *selection,
	uint32 selected_count)
{
	ListCell *cell;

	foreach(cell, pending_transfers)
	{
		BloomPGPendingTransfer *pending = lfirst(cell);
		uint32 selected;

		if (!pending->build_filter)
			continue;
		if (pending->integral_key)
			bloompg_process_pending_integral_chunk(pending, view, selection,
				selected_count);
		else
		{
			for (selected = 0; selected < selected_count; selected++)
				bloompg_filter_insert_vector(pending->filter, view,
					selection[selected], pending->spec.build_attributes);
		}
	}
}

/* Worker packets are already dense vectors.  Build outgoing filters while
 * those vectors are hot in the leader's cache instead of first copying them
 * into the materialization and then scanning the store a second time. */
void
bloompg_process_pending_dense_chunk(List *pending_transfers,
	const BloomPGColumnVector *vectors, uint32 vector_count, uint32 count)
{
	BloomPGColumnStoreChunkView view;
	uint16 selection[BLOOMPG_VECTOR_SIZE];
	uint32 row;

	if (count > BLOOMPG_VECTOR_SIZE)
		elog(ERROR, "invalid bloompg dense filter-build chunk");
	for (row = 0; row < count; row++)
		selection[row] = (uint16) row;
	MemSet(&view, 0, sizeof(view));
	view.count = count;
	view.selected_count = count;
	view.vector_count = vector_count;
	view.selection = selection;
	view.vectors = vectors;
	bloompg_process_pending_chunk(pending_transfers, &view, selection, count);
}

bool
bloompg_contains_materialized_filter(BloomPGFilter *filter,
	const BloomPGColumnStoreChunkView *view, uint32 row,
	const AttrNumber *attributes)
{
	if (filter->kind == BLOOMPG_FILTER_BITMAP && filter->nkeys == 1)
	{
		Datum value;
		bool present = bloompg_column_chunk_get_datum(view, attributes[0], row,
			&value);

		return bloompg_filter_contains_bitmap_value(filter, value, !present);
	}
	return bloompg_filter_contains_vector(filter, view, row, attributes);
}

void
bloompg_insert_materialized_filter(BloomPGFilter *filter,
	const BloomPGColumnStoreChunkView *view, uint32 row,
	const AttrNumber *attributes)
{
	if (filter->kind == BLOOMPG_FILTER_BITMAP && filter->nkeys == 1)
	{
		Datum value;
		bool present = bloompg_column_chunk_get_datum(view, attributes[0], row,
			&value);

		bloompg_filter_insert_bitmap_value(filter, value, !present);
	}
	else
		bloompg_filter_insert_vector(filter, view, row, attributes);
}


/* PostgreSQL slots end at the materialization boundary.  Build every
 * outgoing filter from the completed vectors so the hot loop stays
 * column-oriented. */
void
bloompg_build_pending_from_materialization(List *pending,
	BloomPGColumnStore *store, TupleDesc descriptor)
{
	uint32 chunk_count = bloompg_column_store_chunk_count(store);
	uint32 chunk_index;

	(void) descriptor;
	for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
	{
		BloomPGColumnStoreChunkView view;

		if (!bloompg_column_store_chunk_view(store, chunk_index, &view))
			elog(ERROR, "bloompg lost filter-build vector chunk");
		bloompg_process_pending_chunk(pending, &view, view.selection,
			view.selected_count);
	}
}

void
bloompg_discard_pending_filters(List *pending_transfers)
{
	ListCell *cell;

	foreach(cell, pending_transfers)
	{
		BloomPGPendingTransfer *pending = lfirst(cell);

		/* Candidate storage is owned by the filter's private memory context.
		 * Release it before dropping the filter's final reference, which may
		 * delete that whole context. */
		if (pending->bitmap_candidate_words != NULL)
		{
			pfree(pending->bitmap_candidate_words);
			pending->bitmap_candidate_words = NULL;
		}
		bloompg_filter_free(pending->filter);
		pending->filter = NULL;
	}
}
