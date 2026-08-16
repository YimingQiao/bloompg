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

/* Optimizer-time Apply scan: exact build, then membership-filtered probe. */

static Node *bloompg_create_apply_state(CustomScan *scan);
static void bloompg_begin_apply(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *bloompg_exec_apply(CustomScanState *node);
static void bloompg_end_apply(CustomScanState *node);
static void bloompg_rescan_apply(CustomScanState *node);
static void bloompg_explain_apply(CustomScanState *node,
								  List *ancestors,
								  ExplainState *explain);

const CustomScanMethods bloompg_apply_scan_methods = {
	.CustomName = "BloomPG Apply",
	.CreateCustomScanState = bloompg_create_apply_state
};

static const CustomExecMethods bloompg_apply_exec_methods = {
	.CustomName = "BloomPG Apply",
	.BeginCustomScan = bloompg_begin_apply,
	.ExecCustomScan = bloompg_exec_apply,
	.EndCustomScan = bloompg_end_apply,
	.ReScanCustomScan = bloompg_rescan_apply,
	.ExplainCustomScan = bloompg_explain_apply
};

static Node *
bloompg_create_apply_state(CustomScan *scan)
{
	BloomPGApplyState *state = palloc0(sizeof(BloomPGApplyState));

	(void) scan;
	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &bloompg_apply_exec_methods;
	return (Node *) state;
}

static void
bloompg_parse_apply_spec(BloomPGApplyState *state)
{
	CustomScan *scan = castNode(CustomScan, state->css.ss.ps.plan);
	List *metadata = scan->custom_private;

	if (list_length(metadata) != 8)
		elog(ERROR, "invalid bloompg apply metadata");
	state->spec.token = strtoull(strVal(list_nth(metadata, 0)), NULL, 10);
	state->spec.estimated_rows = strtoll(strVal(list_nth(metadata, 1)), NULL, 10);
	state->spec.nkeys = intVal(list_nth(metadata, 2));
	state->spec.build_is_left = intVal(list_nth(metadata, 3)) != 0;
	state->spec.operators = bloompg_list_to_oid_array(list_nth(metadata, 4),
														state->spec.nkeys);
	state->spec.collations = bloompg_list_to_oid_array(list_nth(metadata, 5),
														 state->spec.nkeys);
	state->spec.build_attributes = bloompg_list_to_attr_array(list_nth(metadata, 6),
															state->spec.nkeys);
	state->spec.probe_attributes = bloompg_list_to_attr_array(list_nth(metadata, 7),
															state->spec.nkeys);
}

static void
bloompg_begin_apply(CustomScanState *custom_state, EState *estate, int eflags)
{
	BloomPGApplyState *state = (BloomPGApplyState *) custom_state;
	CustomScan *scan = castNode(CustomScan, custom_state->ss.ps.plan);
	BloomPGRuntimeFilterEntry *entry;

	bloompg_parse_apply_spec(state);
	state->probe_state = ExecInitNode((Plan *) linitial(scan->custom_plans),
										  estate, eflags);
	state->css.custom_ps = list_make1(state->probe_state);
	entry = bloompg_runtime_find_filter(state->spec.token, estate->es_snapshot);
	if (entry != NULL && entry->filter != NULL && entry->filter->valid)
	{
		state->filter = entry->filter;
		state->filter_ready = true;
	}
}

static void
bloompg_build_filter(BloomPGApplyState *state)
{
	CustomScan *scan = castNode(CustomScan, state->css.ss.ps.plan);
	EState *estate = state->css.ss.ps.state;
	TupleTableSlot *slot;

	state->filter = bloompg_filter_create_bloom(TopTransactionContext,
													 state->spec.estimated_rows,
													 state->spec.nkeys,
													 state->spec.operators,
													 state->spec.collations,
													 state->spec.build_is_left);
	bloompg_runtime_register_filter(state->spec.token, estate->es_snapshot,
									state->filter);
	state->build_state = ExecInitNode((Plan *) lsecond(scan->custom_plans),
										  estate, 0);
	state->css.custom_ps = lappend(state->css.custom_ps, state->build_state);
	for (;;)
	{
		slot = ExecProcNode(state->build_state);
		if (TupIsNull(slot))
			break;
		bloompg_filter_insert_slot(state->filter, slot,
									 state->spec.build_attributes);
	}
	bloompg_filter_set_valid(state->filter);
	state->filter_ready = true;
}

static TupleTableSlot *
bloompg_exec_apply(CustomScanState *custom_state)
{
	BloomPGApplyState *state = (BloomPGApplyState *) custom_state;
	TupleTableSlot *slot;

	if (!state->filter_ready)
		bloompg_build_filter(state);
	/* Strict equality cannot match when the build side contributed no
	 * non-NULL key.  Avoid starting the probe subtree at all; besides saving
	 * work, this preserves PostgreSQL's short-circuit behavior for expressions
	 * on a join branch that native execution would never reach. */
	if (state->filter->inserted == 0)
		return NULL;
	for (;;)
	{
		slot = ExecProcNode(state->probe_state);
		if (TupIsNull(slot))
			return NULL;
		if (bloompg_filter_contains_slot(state->filter, slot,
										state->spec.probe_attributes))
			return ExecCopySlot(custom_state->ss.ps.ps_ResultTupleSlot, slot);
	}
}

static void
bloompg_end_apply(CustomScanState *custom_state)
{
	BloomPGApplyState *state = (BloomPGApplyState *) custom_state;

	if (state->probe_state != NULL)
		ExecEndNode(state->probe_state);
	if (state->build_state != NULL)
		ExecEndNode(state->build_state);
	if (state->filter != NULL)
		bloompg_runtime_release_filter(state->spec.token);
}

static void
bloompg_rescan_apply(CustomScanState *custom_state)
{
	BloomPGApplyState *state = (BloomPGApplyState *) custom_state;

	if (state->probe_state != NULL)
		ExecReScan(state->probe_state);
}

static void
bloompg_explain_apply(CustomScanState *custom_state,
						  List *ancestors,
						  ExplainState *explain)
{
	BloomPGApplyState *state = (BloomPGApplyState *) custom_state;

	(void) ancestors;
	ExplainPropertyInteger("Filter Token", NULL, (int64) state->spec.token, explain);
	ExplainPropertyInteger("Key Count", NULL, state->spec.nkeys, explain);
	if (state->filter != NULL && explain->analyze)
	{
		ExplainPropertyInteger("Build Rows", NULL,
							   (int64) state->filter->inserted, explain);
		ExplainPropertyInteger("Probe Rows", NULL,
							   (int64) state->filter->lookups, explain);
		ExplainPropertyInteger("Rows Rejected", NULL,
							   (int64) state->filter->rejected, explain);
	}
}
