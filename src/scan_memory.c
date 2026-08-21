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
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "port/atomics.h"
#include "utils/snapmgr.h"
#include "bloompg/scan_internal.h"

/* Lazy column-store and exact-empty executor leaves. */

static Node *bloompg_create_memory_state(CustomScan *scan);
static void bloompg_begin_memory(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *bloompg_exec_memory(CustomScanState *node);
static void bloompg_end_memory(CustomScanState *node);
static void bloompg_rescan_memory(CustomScanState *node);
static void bloompg_explain_memory(CustomScanState *node,
								   List *ancestors,
								   ExplainState *explain);
static Node *bloompg_create_empty_state(CustomScan *scan);
static void bloompg_explain_empty(CustomScanState *node,
								  List *ancestors,
								  ExplainState *explain);
static Size bloompg_estimate_memory_dsm(CustomScanState *node,
									 ParallelContext *pcxt);
static void bloompg_initialize_memory_dsm(CustomScanState *node,
									  ParallelContext *pcxt,
									  void *coordinate);
static void bloompg_reinitialize_memory_dsm(CustomScanState *node,
										ParallelContext *pcxt,
										void *coordinate);
static void bloompg_initialize_memory_worker(CustomScanState *node,
										  shm_toc *toc,
										  void *coordinate);

const CustomScanMethods bloompg_memory_scan_methods = {
	.CustomName = "BloomPG Memory",
	.CreateCustomScanState = bloompg_create_memory_state
};

static const CustomExecMethods bloompg_memory_exec_methods = {
	.CustomName = "BloomPG Memory",
	.BeginCustomScan = bloompg_begin_memory,
	.ExecCustomScan = bloompg_exec_memory,
	.EndCustomScan = bloompg_end_memory,
	.ReScanCustomScan = bloompg_rescan_memory,
	.EstimateDSMCustomScan = bloompg_estimate_memory_dsm,
	.InitializeDSMCustomScan = bloompg_initialize_memory_dsm,
	.ReInitializeDSMCustomScan = bloompg_reinitialize_memory_dsm,
	.InitializeWorkerCustomScan = bloompg_initialize_memory_worker,
	.ExplainCustomScan = bloompg_explain_memory
};

const CustomScanMethods bloompg_empty_scan_methods = {
	.CustomName = "BloomPG Empty",
	.CreateCustomScanState = bloompg_create_empty_state
};

static const CustomExecMethods bloompg_empty_exec_methods = {
	.CustomName = "BloomPG Empty",
	.BeginCustomScan = bloompg_begin_memory,
	.ExecCustomScan = bloompg_exec_memory,
	.EndCustomScan = bloompg_end_memory,
	.ReScanCustomScan = bloompg_rescan_memory,
	.EstimateDSMCustomScan = bloompg_estimate_memory_dsm,
	.InitializeDSMCustomScan = bloompg_initialize_memory_dsm,
	.ReInitializeDSMCustomScan = bloompg_reinitialize_memory_dsm,
	.InitializeWorkerCustomScan = bloompg_initialize_memory_worker,
	.ExplainCustomScan = bloompg_explain_empty
};

static Node *
bloompg_create_memory_state(CustomScan *scan)
{
	BloomPGMemoryState *state = palloc0(sizeof(BloomPGMemoryState));

	(void) scan;
	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &bloompg_memory_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	return (Node *) state;
}

static Node *
bloompg_create_empty_state(CustomScan *scan)
{
	BloomPGMemoryState *state = palloc0(sizeof(BloomPGMemoryState));

	(void) scan;
	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &bloompg_empty_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	return (Node *) state;
}

static void
bloompg_begin_memory(CustomScanState *custom_state, EState *estate, int eflags)
{
	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;
	CustomScan *scan = castNode(CustomScan, custom_state->ss.ps.plan);
	List *metadata = scan->custom_private;

	if (list_length(metadata) != 3)
		elog(ERROR, "invalid bloompg memory metadata");
	state->token = strtoull(strVal(list_nth(metadata, 0)), NULL, 10);
	state->planned_rows = strtoull(strVal(list_nth(metadata, 1)), NULL, 10);
	state->parallel_access =
		(BloomPGParallelAccess) intVal(list_nth(metadata, 2));
	if (state->parallel_access != BLOOMPG_PARALLEL_ACCESS_SHARED &&
		state->parallel_access != BLOOMPG_PARALLEL_ACCESS_REPLICATED)
		elog(ERROR, "invalid bloompg parallel memory access mode");
	state->entry = bloompg_runtime_find_materialization(state->token,
										 estate->es_snapshot);
	if (state->entry != NULL && state->entry->store != NULL)
	{
		state->snapshot_reused = true;
		bloompg_column_store_begin_scan(state->entry->store,
			&state->store_scan);
	}
	else
	{
		state->entry = NULL;
		state->snapshot_reused = false;
		state->fallback_state = ExecInitNode((Plan *) linitial(scan->custom_plans),
											 estate, eflags);
		state->css.custom_ps = list_make1(state->fallback_state);
	}
}

static TupleTableSlot *
bloompg_exec_memory(CustomScanState *custom_state)
{
	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;
	bool forward;
	TupleTableSlot *scan_slot = custom_state->ss.ss_ScanTupleSlot;

	if (state->parallel_store != NULL && state->parallel_store->available)
	{
		if (!bloompg_parallel_next_column(state->parallel_store,
			&state->parallel_cursor, scan_slot, state->parallel_access))
			return NULL;
		if (custom_state->ss.ps.ps_ProjInfo != NULL)
		{
			ExprContext *expression_context = custom_state->ss.ps.ps_ExprContext;

			expression_context->ecxt_scantuple = scan_slot;
			return ExecProject(custom_state->ss.ps.ps_ProjInfo);
		}
		return ExecCopySlot(custom_state->ss.ps.ps_ResultTupleSlot, scan_slot);
	}
	if (state->entry == NULL)
	{
		TupleTableSlot *slot = ExecProcNode(state->fallback_state);

		if (TupIsNull(slot))
			return NULL;
		return ExecCopySlot(custom_state->ss.ps.ps_ResultTupleSlot, slot);
	}
	forward = ScanDirectionIsForward(custom_state->ss.ps.state->es_direction);
	if (bloompg_column_store_next(state->entry->store, &state->store_scan,
		forward, custom_state->ss.ss_ScanTupleSlot, NULL))
	{
		if (custom_state->ss.ps.ps_ProjInfo != NULL)
		{
			ExprContext *expression_context = custom_state->ss.ps.ps_ExprContext;

			expression_context->ecxt_scantuple =
				custom_state->ss.ss_ScanTupleSlot;
			return ExecProject(custom_state->ss.ps.ps_ProjInfo);
		}
		return ExecCopySlot(custom_state->ss.ps.ps_ResultTupleSlot,
			custom_state->ss.ss_ScanTupleSlot);
	}
	return NULL;
}

static Size
bloompg_estimate_memory_dsm(CustomScanState *custom_state,
							ParallelContext *pcxt)
{
	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;

	(void) pcxt;
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel memory estimate begin token=" UINT64_FORMAT
			" rows=" UINT64_FORMAT, state->token,
			state->entry != NULL ? state->entry->rows : 0);
	state->parallel_size = bloompg_estimate_parallel_store(state->entry);
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel memory estimate end token=" UINT64_FORMAT
			" bytes=%zu", state->token, state->parallel_size);
	return state->parallel_size;
}

static void
bloompg_initialize_memory_dsm(CustomScanState *custom_state,
							  ParallelContext *pcxt,
							  void *coordinate)
{
	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;

	(void) pcxt;
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel memory DSM begin token=" UINT64_FORMAT,
			state->token);
	bloompg_fill_parallel_store(state->entry,
		(BloomPGParallelStore *) coordinate, state->parallel_size);
	state->parallel_store = (BloomPGParallelStore *) coordinate;
	state->parallel_active = state->parallel_store->available;
	bloompg_parallel_cursor_reset(&state->parallel_cursor);
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel memory DSM end token=" UINT64_FORMAT,
			state->token);
}

static void
bloompg_reinitialize_memory_dsm(CustomScanState *custom_state,
								ParallelContext *pcxt,
								void *coordinate)
{
	BloomPGParallelStore *parallel_store = coordinate;

	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;

	(void) pcxt;
	if (parallel_store->available &&
		state->parallel_access == BLOOMPG_PARALLEL_ACCESS_SHARED)
		pg_atomic_write_u64(&parallel_store->next_item, 0);
	bloompg_parallel_cursor_reset(&state->parallel_cursor);
}

static void
bloompg_initialize_memory_worker(CustomScanState *custom_state,
								  shm_toc *toc,
								  void *coordinate)
{
	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;
	BloomPGParallelStore *parallel_store = coordinate;

	(void) toc;
	if (!parallel_store->available)
		return;
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel memory worker attach token=" UINT64_FORMAT,
			state->token);
	if (state->fallback_state != NULL)
	{
		ExecEndNode(state->fallback_state);
		state->fallback_state = NULL;
		state->css.custom_ps = NIL;
	}
	state->parallel_store = parallel_store;
	state->parallel_active = true;
	bloompg_parallel_cursor_reset(&state->parallel_cursor);
}

static void
bloompg_end_memory(CustomScanState *custom_state)
{
	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;

	if (state->fallback_state != NULL)
		ExecEndNode(state->fallback_state);
	if (state->entry != NULL)
	{
		bloompg_runtime_release_materialization(state->token);
		state->entry = NULL;
	}
	state->parallel_store = NULL;
}

static void
bloompg_rescan_memory(CustomScanState *custom_state)
{
	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;

	if (state->parallel_store != NULL && state->parallel_store->available)
	{
		if (state->parallel_access == BLOOMPG_PARALLEL_ACCESS_REPLICATED)
			bloompg_parallel_cursor_reset(&state->parallel_cursor);
		else
			/* Shared state is reset by ReInitializeDSMCustomScan before workers start. */
			state->parallel_cursor.chunk_valid = false;
	}
	else if (state->entry != NULL)
		bloompg_column_store_rescan(&state->store_scan);
	else if (state->fallback_state != NULL)
		ExecReScan(state->fallback_state);
}

static void
bloompg_explain_memory(CustomScanState *custom_state,
						   List *ancestors,
						   ExplainState *explain)
{
	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;

	(void) ancestors;
	ExplainPropertyInteger("Materialization Token", NULL,
						   (int64) state->token, explain);
	ExplainPropertyInteger("Materialized Rows", NULL,
						   (int64) (state->entry != NULL ? state->entry->rows :
									state->planned_rows), explain);
	ExplainPropertyText("Snapshot Reuse",
							state->parallel_active ?
							(state->parallel_access ==
							 BLOOMPG_PARALLEL_ACCESS_REPLICATED ?
							 "parallel DSM (replicated)" : "parallel DSM (shared)") :
							state->snapshot_reused ? "true" : "fallback",
							explain);
}

static void
bloompg_explain_empty(CustomScanState *custom_state,
					  List *ancestors,
					  ExplainState *explain)
{
	BloomPGMemoryState *state = (BloomPGMemoryState *) custom_state;

	(void) ancestors;
	ExplainPropertyBool("Exact Empty",
		state->parallel_active || state->snapshot_reused, explain);
	ExplainPropertyInteger("Materialization Token", NULL,
		(int64) state->token, explain);
	ExplainPropertyText("Snapshot Reuse",
		state->parallel_active ?
		(state->parallel_access == BLOOMPG_PARALLEL_ACCESS_REPLICATED ?
		 "parallel DSM (replicated)" : "parallel DSM (shared)") :
		state->snapshot_reused ? "true" : "fallback", explain);
}
