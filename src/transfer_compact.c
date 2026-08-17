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
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "tcop/dest.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* Column-store compaction, final filter construction, and TID extraction. */

static void
bloompg_release_consumed_filters(BloomPGRelationState *state)
{
	ListCell *cell;

	foreach(cell, state->filters)
	{
		BloomPGSampleFilter *filter = lfirst(cell);

		bloompg_runtime_release_filter(filter->token);
	}
	state->filters = NIL;
}

/*
 * DuckDB materializes each base source once, then compacts its existing
 * ColumnDataCollection as new cascade filters arrive.  Do the same here:
 * scan the query-local column store, clear rejected rows in its survivor
 * mask, and build all outgoing filters in that same pass.  No detached
 * executor and no second materialization are needed.
 */
uint64
bloompg_compact_materialization(BloomPGRelationState *state,
	List *pending_transfers, TupleDesc *descriptor)
{
	BloomPGRuntimeMaterializationEntry *entry;
	ListCell *cell;
	uint64 rows = 0;
	bool exact_empty = false;
	instr_time started;
	instr_time elapsed;

	entry = bloompg_runtime_find_materialization(state->materialization_token,
		GetActiveSnapshot());
	if (entry == NULL || entry->store == NULL || entry->descriptor == NULL)
		elog(ERROR, "bloompg lost materialization for relation %u", state->rti);
	INSTR_TIME_SET_CURRENT(started);
	state->materialization_used_filters |= state->filters != NIL;
	foreach(cell, state->filters)
	{
		BloomPGSampleFilter *filter = lfirst(cell);

		if (filter->filter == NULL || !filter->filter->valid)
			elog(ERROR, "bloompg compact found an invalid transfer filter");
		if (filter->filter->inserted == 0)
		{
			exact_empty = true;
			break;
		}
	}
	if (exact_empty)
		bloompg_column_store_deactivate_all(entry->store);
	else
	{
		uint32 chunk_index;
		uint32 chunk_count = bloompg_column_store_chunk_count(entry->store);
		uint16 survivors[BLOOMPG_VECTOR_SIZE];

		for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
		{
			BloomPGColumnStoreChunkView view;
			uint32 survivor_count;

			if (!bloompg_column_store_chunk_view(entry->store, chunk_index,
				&view))
				elog(ERROR, "bloompg lost in-memory column chunk");
			survivor_count = view.selected_count;
			if (survivor_count > 0)
				memcpy(survivors, view.selection,
					mul_size(survivor_count, sizeof(uint16)));
			foreach(cell, state->filters)
			{
				BloomPGSampleFilter *filter = lfirst(cell);

				survivor_count = bloompg_filter_select_vector(filter->filter,
					&view, filter->materialized_attributes, survivors,
					survivor_count, survivors);
				if (survivor_count == 0)
					break;
			}
			bloompg_process_pending_chunk(pending_transfers, &view, survivors,
				survivor_count);
			rows += survivor_count;
			bloompg_column_store_set_chunk_selection(entry->store, chunk_index,
				survivors, survivor_count);
		}
		state->column_compactions++;
	}
	if (exact_empty)
		rows = 0;
	if (rows != bloompg_column_store_live_rows(entry->store))
		elog(ERROR, "bloompg compact survivor count changed");
	bloompg_release_consumed_filters(state);
	bloompg_runtime_register_materialization(state->materialization_token,
		GetActiveSnapshot(), entry->store, entry->descriptor, rows);
	*descriptor = entry->descriptor;
	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	state->exact_scans++;
	state->exact_scan_ms += INSTR_TIME_GET_MILLISEC(elapsed);
	state->compactions++;
	state->compact_ms += INSTR_TIME_GET_MILLISEC(elapsed);
	return rows;
}

static bool
bloompg_maybe_convert_to_bitmap(BloomPGPendingTransfer *pending,
									List *all_pending,
									uint64 rows)
{
	const uint64 sign_bit = UINT64CONST(0x8000000000000000);
	uint64 width;
	uint64 max_width = (uint64) bloompg_max_filter_bytes * BITS_PER_BYTE;
	uint64 density_width;
	uint64 aligned_lower;
	uint64 aligned_upper;
	uint64 candidate_words;
	BloomPGFilter *bitmap;
	BloomPGFilter *old_filter;
	MemoryContext parent_context;
	ListCell *cell;
	bool first_reference = true;

	if (!pending->integral_key || !pending->build_filter)
		return true;
	/* No non-NULL build key means the still-empty Bloom is already the exact
	 * representation of an empty semijoin build. */
	if (!pending->has_range ||
		pending->filter->kind != BLOOMPG_FILTER_BLOOM)
		return true;
	width = (uint64) pending->maximum - (uint64) pending->minimum + 1;
	if (width == 0 || width > max_width)
		return false;
	density_width = rows > UINT64_MAX / 128 ? UINT64_MAX : rows * 128;
	if (width > UINT64CONST(8000000) && width > density_width)
		return false;
	if (pending->bitmap_candidate_words == NULL ||
		pending->bitmap_candidate_abandoned)
		return false;
	candidate_words = pending->bitmap_candidate_upper_word -
		pending->bitmap_candidate_lower_word + 1;
	aligned_lower = (pending->bitmap_candidate_lower_word << 6) ^ sign_bit;
	aligned_upper = ((pending->bitmap_candidate_upper_word << 6) | 63) ^
		sign_bit;

	old_filter = pending->filter;
	parent_context = MemoryContextGetParent(old_filter->memory_context);
	bitmap = bloompg_filter_create_bitmap(parent_context,
										 (int64) aligned_lower,
										 (int64) aligned_upper,
										 pending->spec.operators[0],
										 pending->spec.collations[0],
										 pending->spec.build_is_left);
	Assert(bitmap->nbits / 64 == candidate_words);
	memcpy(bitmap->words, pending->bitmap_candidate_words,
		(Size) candidate_words * sizeof(uint64));
	bitmap->inserted = pending->bitmap_candidate_inserted;
	pending->bitmap_one_pass = true;
	pfree(pending->bitmap_candidate_words);
	pending->bitmap_candidate_words = NULL;
	/* A build-key filter can be shared by several destinations.  Replace all
	 * pending references together so no peer keeps a dangling Bloom pointer. */
	foreach(cell, all_pending)
	{
		BloomPGPendingTransfer *peer = lfirst(cell);

		if (peer->filter != old_filter)
			continue;
		peer->filter = first_reference ? bitmap : bloompg_filter_retain(bitmap);
		first_reference = false;
		bloompg_filter_free(old_filter);
	}
	Assert(!first_reference);
	return true;
}

/* DuckDB selects every integral filter representation from the compacted
 * collection's exact ranges, then inserts all filters during one parallel
 * collection scan.  Keep the same phase boundary here: choose each bitmap or
 * fallback Bloom first, then make one column-store pass for every integral
 * owner instead of rescanning once per join edge. */
uint64
bloompg_finalize_pending_filters(List *pending_transfers,
	BloomPGColumnStore *store, TupleDesc descriptor, uint64 rows,
	uint64 *one_pass_bitmap_builds)
{
	ListCell *cell;
	bool needs_insert = false;
	uint64 rows_examined = 0;

	(void) descriptor;
	*one_pass_bitmap_builds = 0;

	foreach(cell, pending_transfers)
	{
		BloomPGPendingTransfer *pending = lfirst(cell);

		if (!pending->build_filter || !pending->integral_key)
			continue;
		(void) bloompg_maybe_convert_to_bitmap(pending, pending_transfers, rows);
		if (pending->bitmap_one_pass)
			(*one_pass_bitmap_builds)++;
		if (pending->has_range && pending->filter->inserted == 0)
			needs_insert = true;
	}
	if (!needs_insert)
		return 0;

	{
		uint32 chunk_index;
		uint32 chunk_count = bloompg_column_store_chunk_count(store);

		for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
		{
			BloomPGColumnStoreChunkView view;
			uint32 selected;

			if (!bloompg_column_store_chunk_view(store, chunk_index, &view))
				elog(ERROR, "bloompg lost final filter-build column chunk");
			for (selected = 0; selected < view.selected_count; selected++)
			{
				uint32 row = view.selection[selected];

				rows_examined++;

				foreach(cell, pending_transfers)
				{
					BloomPGPendingTransfer *pending = lfirst(cell);

					if (pending->build_filter && pending->integral_key &&
						pending->has_range)
						bloompg_insert_materialized_filter(pending->filter,
							&view, row, pending->spec.build_attributes);
				}
			}
		}
	}
	return rows_examined;
}

uint64
bloompg_create_tid_materialization(BloomPGRuntimeMaterializationEntry *source,
									 AttrNumber ctid_attribute,
									 uint64 expected_rows)
{
	MemoryContext old_context;
	BloomPGColumnStore *store;
	TupleDesc descriptor;
	TupleTableSlot *slot;
	TupleTableSlot *output_slot;
	BloomPGColumnStoreScan scan;
	uint64 rows = 0;
	uint64 token;

	if (source == NULL || source->store == NULL ||
		ctid_attribute <= 0 || ctid_attribute > source->descriptor->natts)
		return 0;
	old_context = MemoryContextSwitchTo(TopTransactionContext);
	descriptor = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(descriptor, 1, "ctid", TIDOID, -1, 0);
	MemoryContextSwitchTo(old_context);
	store = bloompg_begin_runtime_store(
		bloompg_column_store_memory_budget(source->store));
	bloompg_column_store_set_descriptor(store, descriptor);
	token = bloompg_runtime_next_token();
	/* Register before copying so the outer error cleanup owns the store even if
	 * tuple extraction subsequently fails. */
	bloompg_runtime_register_materialization(token, GetActiveSnapshot(),
		store, descriptor, 0);
	bloompg_column_store_begin_scan(source->store, &scan);
	slot = MakeSingleTupleTableSlot(source->descriptor, &TTSOpsVirtual);
	output_slot = MakeSingleTupleTableSlot(descriptor, &TTSOpsVirtual);
	while (bloompg_column_store_next(source->store, &scan, true, slot, NULL))
	{
		Datum value;
		Datum values[1];
		bool is_null;
		bool nulls[1] = {false};

		value = slot_getattr(slot, ctid_attribute, &is_null);
		if (is_null)
			elog(ERROR, "bloompg late materialization found a NULL ctid");
		values[0] = value;
		ExecClearTuple(output_slot);
		output_slot->tts_values[0] = values[0];
		output_slot->tts_isnull[0] = nulls[0];
		ExecStoreVirtualTuple(output_slot);
		bloompg_column_store_append_slot(store, output_slot);
		rows++;
		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);
	ExecDropSingleTupleTableSlot(output_slot);
	if (rows != expected_rows)
		elog(ERROR, "bloompg late materialization row count changed");
	bloompg_runtime_register_materialization(token, GetActiveSnapshot(),
		store, descriptor, rows);
	return token;
}
