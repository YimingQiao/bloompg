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

/* Late-materialized CTID executor leaf and heap fetch boundary. */

static Node *bloompg_create_tid_state(CustomScan *scan);
static void bloompg_begin_tid(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *bloompg_exec_tid(CustomScanState *node);
static void bloompg_end_tid(CustomScanState *node);
static void bloompg_rescan_tid(CustomScanState *node);
static void bloompg_explain_tid(CustomScanState *node,
								List *ancestors,
								ExplainState *explain);
static Size bloompg_estimate_tid_dsm(CustomScanState *node,
								  ParallelContext *pcxt);
static void bloompg_initialize_tid_dsm(CustomScanState *node,
								   ParallelContext *pcxt,
								   void *coordinate);
static void bloompg_reinitialize_tid_dsm(CustomScanState *node,
									 ParallelContext *pcxt,
									 void *coordinate);
static void bloompg_initialize_tid_worker(CustomScanState *node,
									   shm_toc *toc,
									   void *coordinate);

const CustomScanMethods bloompg_tid_scan_methods = {
	.CustomName = "BloomPG TID",
	.CreateCustomScanState = bloompg_create_tid_state
};

static const CustomExecMethods bloompg_tid_exec_methods = {
	.CustomName = "BloomPG TID",
	.BeginCustomScan = bloompg_begin_tid,
	.ExecCustomScan = bloompg_exec_tid,
	.EndCustomScan = bloompg_end_tid,
	.ReScanCustomScan = bloompg_rescan_tid,
	.EstimateDSMCustomScan = bloompg_estimate_tid_dsm,
	.InitializeDSMCustomScan = bloompg_initialize_tid_dsm,
	.ReInitializeDSMCustomScan = bloompg_reinitialize_tid_dsm,
	.InitializeWorkerCustomScan = bloompg_initialize_tid_worker,
	.ExplainCustomScan = bloompg_explain_tid
};

static Node *
bloompg_create_tid_state(CustomScan *scan)
{
	BloomPGTidState *state = palloc0(sizeof(BloomPGTidState));

	(void) scan;
	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &bloompg_tid_exec_methods;
	return (Node *) state;
}

static void
bloompg_begin_tid(CustomScanState *custom_state, EState *estate, int eflags)
{
	BloomPGTidState *state = (BloomPGTidState *) custom_state;
	CustomScan *scan = castNode(CustomScan, custom_state->ss.ps.plan);
	List *metadata = scan->custom_private;

	if (list_length(metadata) != 2 || custom_state->ss.ss_currentRelation == NULL)
		elog(ERROR, "invalid bloompg TID scan metadata");
	state->token = strtoull(strVal(list_nth(metadata, 0)), NULL, 10);
	state->planned_rows = strtoull(strVal(list_nth(metadata, 1)), NULL, 10);
	state->entry = bloompg_runtime_find_materialization(state->token,
		estate->es_snapshot);
	if (state->entry != NULL && state->entry->store != NULL)
	{
		bloompg_column_store_begin_scan(state->entry->store,
			&state->store_scan, eflags);
		state->tid_slot = MakeSingleTupleTableSlot(state->entry->descriptor,
			&TTSOpsVirtual);
		state->heap_slot = table_slot_create(custom_state->ss.ss_currentRelation,
			&estate->es_tupleTable);
	}
	else
	{
		state->fallback_state = ExecInitNode((Plan *) linitial(scan->custom_plans),
			estate, eflags);
		state->css.custom_ps = list_make1(state->fallback_state);
	}
}

static TupleTableSlot *
bloompg_exec_tid(CustomScanState *custom_state)
{
	BloomPGTidState *state = (BloomPGTidState *) custom_state;
	ScanState *scan_state = &custom_state->ss;
	bool forward;

	if (state->parallel_store != NULL && state->parallel_store->available)
	{
		for (;;)
		{
			TupleTableSlot *source_slot;
			bool is_null;
			Datum datum;

			if (state->parallel_store->format == BLOOMPG_PARALLEL_STORE_COLUMNS)
			{
				if (!bloompg_parallel_next_column(state->parallel_store,
					&state->parallel_cursor, state->tid_slot))
					return NULL;
				source_slot = state->tid_slot;
			}
			else
			{
				MinimalTuple tuple =
					bloompg_parallel_next_tuple(state->parallel_store);

				if (tuple == NULL)
					return NULL;
				if (state->parallel_slot == NULL)
					state->parallel_slot = MakeSingleTupleTableSlot(
						state->tid_slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
				ExecClearTuple(state->parallel_slot);
				ExecStoreMinimalTuple(tuple, state->parallel_slot, false);
				source_slot = state->parallel_slot;
			}
			datum = slot_getattr(source_slot, 1, &is_null);
			if (is_null)
				elog(ERROR, "bloompg TID materialization contains NULL");
			ExecClearTuple(state->heap_slot);
			if (table_tuple_fetch_row_version(scan_state->ss_currentRelation,
				DatumGetItemPointer(datum), scan_state->ps.state->es_snapshot,
				state->heap_slot))
			{
				ExprContext *expression_context = scan_state->ps.ps_ExprContext;

				ExecCopySlot(scan_state->ss_ScanTupleSlot, state->heap_slot);
				expression_context->ecxt_scantuple = scan_state->ss_ScanTupleSlot;
				ExecClearTuple(source_slot);
				if (scan_state->ps.ps_ProjInfo != NULL)
					return ExecProject(scan_state->ps.ps_ProjInfo);
				return ExecCopySlot(scan_state->ps.ps_ResultTupleSlot,
					scan_state->ss_ScanTupleSlot);
			}
			ExecClearTuple(source_slot);
		}
	}
	if (state->entry == NULL)
	{
		TupleTableSlot *slot = ExecProcNode(state->fallback_state);

		if (TupIsNull(slot))
			return NULL;
		return ExecCopySlot(custom_state->ss.ps.ps_ResultTupleSlot, slot);
	}
	forward = ScanDirectionIsForward(scan_state->ps.state->es_direction);
	while (bloompg_column_store_next(state->entry->store, &state->store_scan,
		forward, state->tid_slot, NULL))
	{
		bool is_null;
		Datum datum = slot_getattr(state->tid_slot, 1, &is_null);

		if (is_null)
			elog(ERROR, "bloompg TID materialization contains NULL");
		ExecClearTuple(state->heap_slot);
		if (table_tuple_fetch_row_version(scan_state->ss_currentRelation,
			DatumGetItemPointer(datum), scan_state->ps.state->es_snapshot,
			state->heap_slot))
		{
			ExprContext *expression_context = scan_state->ps.ps_ExprContext;

			ExecCopySlot(scan_state->ss_ScanTupleSlot, state->heap_slot);
			expression_context->ecxt_scantuple = scan_state->ss_ScanTupleSlot;
			ExecClearTuple(state->tid_slot);
			if (scan_state->ps.ps_ProjInfo != NULL)
				return ExecProject(scan_state->ps.ps_ProjInfo);
			return ExecCopySlot(scan_state->ps.ps_ResultTupleSlot,
				scan_state->ss_ScanTupleSlot);
		}
		ExecClearTuple(state->tid_slot);
	}
	return NULL;
}

static Size
bloompg_estimate_tid_dsm(CustomScanState *custom_state,
						 ParallelContext *pcxt)
{
	BloomPGTidState *state = (BloomPGTidState *) custom_state;

	(void) pcxt;
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel TID estimate begin token=" UINT64_FORMAT
			" rows=" UINT64_FORMAT, state->token,
			state->entry != NULL ? state->entry->rows : 0);
	state->parallel_size = bloompg_estimate_parallel_store(state->entry);
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel TID estimate end token=" UINT64_FORMAT
			" bytes=%zu", state->token, state->parallel_size);
	return state->parallel_size;
}

static void
bloompg_initialize_tid_dsm(CustomScanState *custom_state,
						   ParallelContext *pcxt,
						   void *coordinate)
{
	BloomPGTidState *state = (BloomPGTidState *) custom_state;

	(void) pcxt;
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel TID DSM begin token=" UINT64_FORMAT,
			state->token);
	bloompg_fill_parallel_store(state->entry,
		(BloomPGParallelStore *) coordinate, state->parallel_size);
	state->parallel_store = (BloomPGParallelStore *) coordinate;
	state->parallel_active = state->parallel_store->available;
	state->parallel_cursor.chunk_valid = false;
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel TID DSM end token=" UINT64_FORMAT,
			state->token);
}

static void
bloompg_reinitialize_tid_dsm(CustomScanState *custom_state,
							 ParallelContext *pcxt,
							 void *coordinate)
{
	BloomPGParallelStore *parallel_store = coordinate;

	BloomPGTidState *state = (BloomPGTidState *) custom_state;

	(void) pcxt;
	if (parallel_store->available)
		pg_atomic_write_u64(&parallel_store->next_item, 0);
	state->parallel_cursor.chunk_valid = false;
}

static void
bloompg_initialize_tid_worker(CustomScanState *custom_state,
							   shm_toc *toc,
							   void *coordinate)
{
	BloomPGTidState *state = (BloomPGTidState *) custom_state;
	BloomPGParallelStore *parallel_store = coordinate;
	TupleDesc descriptor;

	(void) toc;
	if (!parallel_store->available)
		return;
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel TID worker attach token=" UINT64_FORMAT,
			state->token);
	if (state->fallback_state != NULL)
	{
		ExecEndNode(state->fallback_state);
		state->fallback_state = NULL;
		state->css.custom_ps = NIL;
	}
	state->parallel_store = parallel_store;
	state->parallel_active = true;
	state->parallel_cursor.chunk_valid = false;
	if (state->tid_slot == NULL)
	{
		descriptor = CreateTemplateTupleDesc(1);
		TupleDescInitEntry(descriptor, 1, "ctid", TIDOID, -1, 0);
		state->tid_slot = MakeSingleTupleTableSlot(descriptor,
			&TTSOpsVirtual);
	}
	if (parallel_store->format == BLOOMPG_PARALLEL_STORE_TUPLES &&
		state->parallel_slot == NULL)
		state->parallel_slot = MakeSingleTupleTableSlot(
			state->tid_slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
	if (state->heap_slot == NULL)
		state->heap_slot = table_slot_create(custom_state->ss.ss_currentRelation,
			&custom_state->ss.ps.state->es_tupleTable);
}

static void
bloompg_end_tid(CustomScanState *custom_state)
{
	BloomPGTidState *state = (BloomPGTidState *) custom_state;

	if (state->fallback_state != NULL)
		ExecEndNode(state->fallback_state);
	if (state->tid_slot != NULL)
		ExecDropSingleTupleTableSlot(state->tid_slot);
	if (state->parallel_slot != NULL)
		ExecDropSingleTupleTableSlot(state->parallel_slot);
	if (state->entry != NULL && state->entry->store != NULL)
		bloompg_column_store_end_scan(&state->store_scan);
	if (state->entry != NULL)
	{
		bloompg_runtime_release_materialization(state->token);
		state->entry = NULL;
	}
	state->parallel_store = NULL;
}

static void
bloompg_rescan_tid(CustomScanState *custom_state)
{
	BloomPGTidState *state = (BloomPGTidState *) custom_state;

	if (state->parallel_store != NULL && state->parallel_store->available)
	{
		if (!IsParallelWorker())
			pg_atomic_write_u64(&state->parallel_store->next_item, 0);
		state->parallel_cursor.chunk_valid = false;
	}
	else if (state->entry != NULL)
		bloompg_column_store_rescan(state->entry->store, &state->store_scan);
	else if (state->fallback_state != NULL)
		ExecReScan(state->fallback_state);
}

static void
bloompg_explain_tid(CustomScanState *custom_state,
						List *ancestors,
						ExplainState *explain)
{
	BloomPGTidState *state = (BloomPGTidState *) custom_state;

	(void) ancestors;
	ExplainPropertyInteger("TID Token", NULL, (int64) state->token, explain);
	ExplainPropertyInteger("Survivor TIDs", NULL,
		(int64) (state->entry != NULL ? state->entry->rows : state->planned_rows),
		explain);
	ExplainPropertyText("Snapshot Reuse",
		state->parallel_active ?
		"parallel DSM" : state->entry != NULL ? "true" : "fallback", explain);
}
