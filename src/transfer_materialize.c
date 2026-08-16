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

/*
 * Optimizer execution runs inside an internal subtransaction, but accepted
 * materializations are consumed by the executor after that subtransaction
 * commits.  A tuplestore remembers CurrentResourceOwner at creation time and
 * uses that owner if it later spills, so merely allocating the state in
 * TopTransactionContext is not sufficient.  Capture the planner's parent
 * owner here and restore both globals even if initialization throws.
 */
BloomPGColumnStore *
bloompg_begin_runtime_store(ResourceOwner runtime_owner)
{
	Assert(runtime_owner != NULL);
	return bloompg_column_store_create(TopTransactionContext, runtime_owner,
		mul_size((Size) bloompg_materialization_memory_kb, (Size) 1024));
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
static void
bloompg_pending_bitmap_add(BloomPGPendingTransfer *pending, int64 value)
{
	const uint64 sign_bit = UINT64CONST(0x8000000000000000);
	const uint64 maximum_word = UINT64_MAX >> 6;
	const uint64 initial_words = 64;
	uint64 biased = ((uint64) value) ^ sign_bit;
	uint64 wanted_word = biased >> 6;
	uint64 max_words = ((uint64) bloompg_max_filter_bytes *
		BITS_PER_BYTE) / 64;
	uint64 lower_word;
	uint64 upper_word;
	uint64 word_count;
	uint64 *words;

	if (!pending->build_filter || pending->bitmap_candidate_abandoned)
		return;
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
			return;
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
			return;
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
	pending->bitmap_candidate_inserted++;
}

bool
bloompg_process_pending_slot(List *pending_transfers, TupleTableSlot *slot)
{
	ListCell *cell;

	foreach(cell, pending_transfers)
	{
		BloomPGPendingTransfer *pending = lfirst(cell);

		if (!pending->build_filter)
			continue;
		if (!pending->integral_key)
			bloompg_filter_insert_slot(pending->filter, slot,
									 pending->spec.build_attributes);
		if (pending->integral_key)
		{
			bool is_null;
			Datum value = slot_getattr(slot, pending->spec.build_attributes[0],
									 &is_null);
			int64 integral;

			if (!is_null && bloompg_integral_value(value,
												 pending->filter->keys[0].build_type,
												 &integral))
			{
				if (!pending->has_range)
					pending->minimum = pending->maximum = integral;
				else
				{
					pending->minimum = Min(pending->minimum, integral);
					pending->maximum = Max(pending->maximum, integral);
				}
				pending->has_range = true;
				bloompg_pending_bitmap_add(pending, integral);
			}
		}
	}
	return true;
}

void
bloompg_process_pending_columns(List *pending_transfers,
	const BloomPGColumnStoreChunkView *view, uint32 row)
{
	ListCell *cell;

	foreach(cell, pending_transfers)
	{
		BloomPGPendingTransfer *pending = lfirst(cell);
		AttrNumber attribute = pending->spec.build_attributes[0];
		Datum value;
		int64 integral;

		if (!pending->build_filter)
			continue;
		if (!pending->integral_key)
			bloompg_filter_insert_vector(pending->filter, view, row,
				pending->spec.build_attributes);
		if (!pending->integral_key)
			continue;
		if (!AttributeNumberIsValid(attribute))
			elog(ERROR, "invalid bloompg column-store range attribute");
		if (bloompg_column_chunk_get_datum(view, attribute, row, &value) &&
			bloompg_integral_value(value,
			pending->filter->keys[0].build_type, &integral))
		{
			if (!pending->has_range)
				pending->minimum = pending->maximum = integral;
			else
			{
				pending->minimum = Min(pending->minimum, integral);
				pending->maximum = Max(pending->maximum, integral);
			}
			pending->has_range = true;
			bloompg_pending_bitmap_add(pending, integral);
		}
	}
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
 * outgoing filter from the completed flat vectors so the hot loop remains
 * column-oriented; spilled stores retain the equivalent slot fallback. */
bool
bloompg_build_pending_from_materialization(List *pending,
	BloomPGColumnStore *store, TupleDesc descriptor)
{
	if (!bloompg_column_store_spilled(store))
	{
		uint32 chunk_count = bloompg_column_store_chunk_count(store);
		uint32 chunk_index;

		for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
		{
			BloomPGColumnStoreChunkView view;
			uint32 selected;

			if (!bloompg_column_store_chunk_view(store, chunk_index, &view))
				elog(ERROR, "bloompg lost filter-build vector chunk");
			for (selected = 0; selected < view.selected_count; selected++)
				bloompg_process_pending_columns(pending, &view,
					view.selection[selected]);
		}
		return true;
	}
	else
	{
		BloomPGColumnStoreScan scan;
		TupleTableSlot *slot = MakeSingleTupleTableSlot(descriptor,
			&TTSOpsVirtual);

		bloompg_column_store_begin_scan(store, &scan, EXEC_FLAG_REWIND);
		while (bloompg_column_store_next(store, &scan, true, slot, NULL))
		{
			bloompg_process_pending_slot(pending, slot);
			ExecClearTuple(slot);
		}
		ExecDropSingleTupleTableSlot(slot);
		bloompg_column_store_end_scan(&scan);
		return false;
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
