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

/* Native-path filter wrapper, prefilter mode, and DSM filter transport. */

static Node *bloompg_create_filter_state(CustomScan *scan);
static void bloompg_begin_filter(CustomScanState *node, EState *estate,
								 int eflags);
static TupleTableSlot *bloompg_exec_filter(CustomScanState *node);
static void bloompg_end_filter(CustomScanState *node);
static void bloompg_rescan_filter(CustomScanState *node);
static Size bloompg_estimate_filter_dsm(CustomScanState *node,
									  ParallelContext *pcxt);
static void bloompg_initialize_filter_dsm(CustomScanState *node,
									 ParallelContext *pcxt,
									 void *coordinate);
static void bloompg_reinitialize_filter_dsm(CustomScanState *node,
									   ParallelContext *pcxt,
									   void *coordinate);
static void bloompg_initialize_filter_worker(CustomScanState *node,
										shm_toc *toc,
										void *coordinate);
static void bloompg_explain_filter(CustomScanState *node,
								   List *ancestors,
								   ExplainState *explain);

const CustomScanMethods bloompg_filter_scan_methods = {
	.CustomName = "BloomPG Filter",
	.CreateCustomScanState = bloompg_create_filter_state
};

const CustomScanMethods bloompg_prefilter_scan_methods = {
	.CustomName = "BloomPG Prefilter",
	.CreateCustomScanState = bloompg_create_filter_state
};

static const CustomExecMethods bloompg_prefilter_exec_methods = {
	.CustomName = "BloomPG Prefilter",
	.BeginCustomScan = bloompg_begin_filter,
	.ExecCustomScan = bloompg_exec_filter,
	.EndCustomScan = bloompg_end_filter,
	.ReScanCustomScan = bloompg_rescan_filter,
	.EstimateDSMCustomScan = bloompg_estimate_filter_dsm,
	.InitializeDSMCustomScan = bloompg_initialize_filter_dsm,
	.ReInitializeDSMCustomScan = bloompg_reinitialize_filter_dsm,
	.InitializeWorkerCustomScan = bloompg_initialize_filter_worker,
	.ExplainCustomScan = bloompg_explain_filter
};

static const CustomExecMethods bloompg_filter_exec_methods = {
	.CustomName = "BloomPG Filter",
	.BeginCustomScan = bloompg_begin_filter,
	.ExecCustomScan = bloompg_exec_filter,
	.EndCustomScan = bloompg_end_filter,
	.ReScanCustomScan = bloompg_rescan_filter,
	.EstimateDSMCustomScan = bloompg_estimate_filter_dsm,
	.InitializeDSMCustomScan = bloompg_initialize_filter_dsm,
	.ReInitializeDSMCustomScan = bloompg_reinitialize_filter_dsm,
	.InitializeWorkerCustomScan = bloompg_initialize_filter_worker,
	.ExplainCustomScan = bloompg_explain_filter
};

static Node *
bloompg_create_filter_state(CustomScan *scan)
{
	BloomPGFilterState *state = palloc0(sizeof(BloomPGFilterState));

	NodeSetTag(&state->css, T_CustomScanState);
	state->prefilter = scan->methods == &bloompg_prefilter_scan_methods;
	state->css.methods = state->prefilter ? &bloompg_prefilter_exec_methods :
		&bloompg_filter_exec_methods;
	return (Node *) state;
}

static void
bloompg_begin_filter(CustomScanState *custom_state, EState *estate, int eflags)
{
	BloomPGFilterState *state = (BloomPGFilterState *) custom_state;
	CustomScan *scan = castNode(CustomScan, custom_state->ss.ps.plan);
	ListCell *cell;

	if (list_length(scan->custom_plans) != 1)
		elog(ERROR, "invalid bloompg filter child plan");
	state->fallback_state = ExecInitNode((Plan *) linitial(scan->custom_plans),
		estate, eflags);
	state->css.custom_ps = list_make1(state->fallback_state);
	foreach(cell, scan->custom_private)
	{
		List *metadata = lfirst_node(List, cell);
		BloomPGExecFilter *exec_filter;
		BloomPGRuntimeFilterEntry *entry;

		if (list_length(metadata) != 3)
			elog(ERROR, "invalid bloompg direct-filter metadata");
		exec_filter = palloc0(sizeof(BloomPGExecFilter));
		exec_filter->token = strtoull(strVal(list_nth(metadata, 0)), NULL, 10);
		exec_filter->nkeys = intVal(list_nth(metadata, 1));
		if (exec_filter->nkeys <= 0)
			elog(ERROR, "invalid bloompg direct-filter key count");
		exec_filter->attributes = bloompg_list_to_attr_array(
			(List *) list_nth(metadata, 2), exec_filter->nkeys);
		entry = bloompg_runtime_find_filter(exec_filter->token,
			estate->es_snapshot);
		if (entry != NULL && entry->filter != NULL && entry->filter->valid)
			exec_filter->filter = entry->filter;
		state->filters = lappend(state->filters, exec_filter);
	}
}

static Size
bloompg_parallel_filter_header_size(int nfilters)
{
	if (nfilters < 0 ||
		nfilters > (int) (MaxAllocSize / sizeof(BloomPGParallelFilterEntry)))
		elog(ERROR, "bloompg has too many parallel direct filters");
	return MAXALIGN(add_size(offsetof(BloomPGParallelFilterStore, entries),
		mul_size((Size) nfilters, sizeof(BloomPGParallelFilterEntry))));
}

static Size
bloompg_estimate_filter_dsm(CustomScanState *custom_state,
							ParallelContext *pcxt)
{
	BloomPGFilterState *state = (BloomPGFilterState *) custom_state;
	Size size = bloompg_parallel_filter_header_size(list_length(state->filters));
	ListCell *cell;

	(void) pcxt;
	foreach(cell, state->filters)
	{
		BloomPGExecFilter *exec_filter = lfirst(cell);
		BloomPGFilter *filter = exec_filter->filter;

		if (filter == NULL || !filter->valid)
			continue;
		size = MAXALIGN(size);
		size = add_size(size,
			mul_size((Size) filter->nkeys, sizeof(BloomPGParallelFilterKey)));
		size = MAXALIGN(size);
		size = add_size(size,
			mul_size((Size) (filter->nbits / 64), sizeof(uint64)));
	}
	state->parallel_size = size;
	return size;
}

static void
bloompg_initialize_filter_dsm(CustomScanState *custom_state,
							  ParallelContext *pcxt,
							  void *coordinate)
{
	BloomPGFilterState *state = (BloomPGFilterState *) custom_state;
	BloomPGParallelFilterStore *store = coordinate;
	Size used = bloompg_parallel_filter_header_size(list_length(state->filters));
	ListCell *cell;
	int index = 0;

	(void) pcxt;
	MemSet(store, 0, state->parallel_size);
	store->nfilters = list_length(state->filters);
	store->total_size = state->parallel_size;
	foreach(cell, state->filters)
	{
		BloomPGExecFilter *exec_filter = lfirst(cell);
		BloomPGFilter *filter = exec_filter->filter;
		BloomPGParallelFilterEntry *entry = &store->entries[index++];
		BloomPGParallelFilterKey *keys;
		Size words_size;
		int key;

		if (filter == NULL || !filter->valid)
			continue;
		entry->active = true;
		entry->kind = (int) filter->kind;
		entry->nkeys = filter->nkeys;
		entry->hash_functions = filter->hash_functions;
		entry->nbits = filter->nbits;
		entry->bit_mask = filter->bit_mask;
		entry->inserted = filter->inserted;
		entry->bitmap_lower = filter->bitmap_lower;
		entry->bitmap_upper = filter->bitmap_upper;
		used = MAXALIGN(used);
		entry->keys_offset = used;
		keys = (BloomPGParallelFilterKey *) ((char *) store + used);
		used = add_size(used,
			mul_size((Size) filter->nkeys, sizeof(BloomPGParallelFilterKey)));
		for (key = 0; key < filter->nkeys; key++)
		{
			keys[key].probe_type = filter->keys[key].probe_type;
			keys[key].collation = filter->keys[key].collation;
			keys[key].probe_hash_proc = filter->keys[key].probe_hash_proc;
		}
		used = MAXALIGN(used);
		entry->words_offset = used;
		words_size = mul_size((Size) (filter->nbits / 64), sizeof(uint64));
		used = add_size(used, words_size);
		if (used > state->parallel_size)
			elog(ERROR, "bloompg parallel direct filter exceeded its DSM estimate");
		memcpy((char *) store + entry->words_offset, filter->words, words_size);
	}
	if (used > state->parallel_size)
		elog(ERROR, "bloompg parallel direct-filter metadata overflow");
	store->available = true;
	state->parallel_store = store;
	state->parallel_active = true;
}

static void
bloompg_reinitialize_filter_dsm(CustomScanState *custom_state,
								ParallelContext *pcxt,
								void *coordinate)
{
	(void) custom_state;
	(void) pcxt;
	(void) coordinate;
}

static void
bloompg_initialize_filter_worker(CustomScanState *custom_state,
								 shm_toc *toc,
								 void *coordinate)
{
	BloomPGFilterState *state = (BloomPGFilterState *) custom_state;
	BloomPGParallelFilterStore *store = coordinate;
	ListCell *cell;
	int index = 0;

	(void) toc;
	if (store == NULL || !store->available ||
		store->nfilters != list_length(state->filters) ||
		store->total_size < bloompg_parallel_filter_header_size(store->nfilters))
		return;
	foreach(cell, state->filters)
	{
		BloomPGExecFilter *exec_filter = lfirst(cell);
		BloomPGParallelFilterEntry *entry = &store->entries[index++];
		BloomPGParallelFilterKey *serialized_keys;
		BloomPGFilter *filter;
		Size keys_size;
		Size words_size;
		int key;

		if (!entry->active)
			continue;
		if (entry->nkeys != exec_filter->nkeys || entry->nkeys <= 0 ||
			(entry->kind != BLOOMPG_FILTER_BLOOM &&
			 entry->kind != BLOOMPG_FILTER_BITMAP) ||
			entry->nbits < 64 || entry->nbits % 64 != 0)
			elog(ERROR, "invalid bloompg parallel direct-filter metadata");
		keys_size = mul_size((Size) entry->nkeys,
			sizeof(BloomPGParallelFilterKey));
		words_size = mul_size((Size) (entry->nbits / 64), sizeof(uint64));
		if (entry->keys_offset > store->total_size ||
			keys_size > store->total_size - entry->keys_offset ||
			entry->words_offset > store->total_size ||
			words_size > store->total_size - entry->words_offset)
			elog(ERROR, "invalid bloompg parallel direct-filter offsets");
		serialized_keys = (BloomPGParallelFilterKey *)
			((char *) store + entry->keys_offset);
		filter = palloc0(sizeof(BloomPGFilter));
		filter->kind = (BloomPGFilterKind) entry->kind;
		filter->refcount = 1;
		filter->nkeys = entry->nkeys;
		filter->keys = palloc0_array(BloomPGFilterKey, entry->nkeys);
		filter->valid = true;
		filter->nbits = entry->nbits;
			filter->bit_mask = entry->bit_mask;
			filter->inserted = entry->inserted;
		filter->words = (uint64 *) ((char *) store + entry->words_offset);
		filter->hash_functions = entry->hash_functions;
		filter->bitmap_lower = entry->bitmap_lower;
		filter->bitmap_upper = entry->bitmap_upper;
		for (key = 0; key < entry->nkeys; key++)
		{
			filter->keys[key].probe_type = serialized_keys[key].probe_type;
			filter->keys[key].collation = serialized_keys[key].collation;
			filter->keys[key].probe_hash_proc =
				serialized_keys[key].probe_hash_proc;
			if (OidIsValid(filter->keys[key].probe_hash_proc))
				fmgr_info(filter->keys[key].probe_hash_proc,
					&filter->keys[key].probe_hash);
		}
		exec_filter->filter = filter;
	}
	state->parallel_store = store;
	state->parallel_active = true;
}

static TupleTableSlot *
bloompg_exec_prefilter(BloomPGFilterState *state)
{
	CustomScanState *custom_state = &state->css;
	ScanState *output_state = &custom_state->ss;
	SeqScanState *scan_state = castNode(SeqScanState, state->fallback_state);
	TableScanDesc scan_descriptor = scan_state->ss.ss_currentScanDesc;
	TupleTableSlot *scan_slot = scan_state->ss.ss_ScanTupleSlot;
	ExprContext *expression_context = scan_state->ss.ps.ps_ExprContext;
	ListCell *cell;

	foreach(cell, state->filters)
	{
		BloomPGExecFilter *exec_filter = lfirst(cell);

		if (exec_filter->filter != NULL && exec_filter->filter->valid &&
			exec_filter->filter->inserted == 0)
			return NULL;
	}
	if (scan_descriptor == NULL)
	{
		scan_descriptor = table_beginscan(
			scan_state->ss.ss_currentRelation,
			scan_state->ss.ps.state->es_snapshot, 0, NULL);
		scan_state->ss.ss_currentScanDesc = scan_descriptor;
	}
	while (table_scan_getnextslot(scan_descriptor,
			scan_state->ss.ps.state->es_direction, scan_slot))
	{
		TupleTableSlot *result_slot;
		bool accepted = true;

		ResetExprContext(expression_context);
		expression_context->ecxt_scantuple = scan_slot;
		foreach(cell, state->filters)
		{
			BloomPGExecFilter *exec_filter = lfirst(cell);
			bool contains = true;

			if (exec_filter->filter != NULL)
			{
				if (exec_filter->filter->kind == BLOOMPG_FILTER_BITMAP &&
					exec_filter->nkeys == 1)
				{
					bool is_null;
					Datum value = slot_getattr(scan_slot,
						exec_filter->attributes[0], &is_null);

					contains = bloompg_filter_contains_bitmap_value(
						exec_filter->filter, value, is_null);
				}
				else
					contains = bloompg_filter_contains_slot(exec_filter->filter,
						scan_slot, exec_filter->attributes);
			}
			if (!contains)
			{
				accepted = false;
				state->rejected++;
				break;
			}
		}
		if (!accepted)
			continue;
		if (scan_state->ss.ps.qual != NULL &&
			!ExecQual(scan_state->ss.ps.qual, expression_context))
			continue;
		result_slot = scan_state->ss.ps.ps_ProjInfo != NULL ?
			ExecProject(scan_state->ss.ps.ps_ProjInfo) : scan_slot;
		ExecCopySlot(output_state->ss_ScanTupleSlot, result_slot);
		output_state->ps.ps_ExprContext->ecxt_scantuple =
			output_state->ss_ScanTupleSlot;
		if (output_state->ps.ps_ProjInfo != NULL)
			return ExecProject(output_state->ps.ps_ProjInfo);
		return ExecCopySlot(output_state->ps.ps_ResultTupleSlot,
			output_state->ss_ScanTupleSlot);
	}
	return NULL;
}

static TupleTableSlot *
bloompg_exec_filter(CustomScanState *custom_state)
{
	BloomPGFilterState *state = (BloomPGFilterState *) custom_state;
	ScanState *scan_state = &custom_state->ss;

	if (state->prefilter && IsA(state->fallback_state, SeqScanState))
		return bloompg_exec_prefilter(state);

	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(state->fallback_state);
		ListCell *cell;
		bool accepted = true;

		if (TupIsNull(slot))
			return NULL;
		foreach(cell, state->filters)
		{
			BloomPGExecFilter *exec_filter = lfirst(cell);
			bool contains = true;

			/* A stale prepared plan or a parallel worker has no backend-local
			 * registry entry.  Ignoring the optional semijoin filter yields the
			 * original relation and is therefore the safe fallback. */
			if (exec_filter->filter != NULL)
			{
				if (exec_filter->filter->kind == BLOOMPG_FILTER_BITMAP &&
					exec_filter->nkeys == 1)
				{
					bool is_null;
					Datum value = slot_getattr(slot, exec_filter->attributes[0],
						&is_null);

					contains = bloompg_filter_contains_bitmap_value(
						exec_filter->filter, value, is_null);
				}
				else
					contains = bloompg_filter_contains_slot(exec_filter->filter,
						slot, exec_filter->attributes);
			}
			if (!contains)
			{
				accepted = false;
				state->rejected++;
				break;
			}
		}
		if (!accepted)
			continue;
		ExecCopySlot(scan_state->ss_ScanTupleSlot, slot);
		scan_state->ps.ps_ExprContext->ecxt_scantuple =
			scan_state->ss_ScanTupleSlot;
		if (scan_state->ps.ps_ProjInfo != NULL)
			return ExecProject(scan_state->ps.ps_ProjInfo);
		return ExecCopySlot(scan_state->ps.ps_ResultTupleSlot,
			scan_state->ss_ScanTupleSlot);
	}
}

static void
bloompg_end_filter(CustomScanState *custom_state)
{
	BloomPGFilterState *state = (BloomPGFilterState *) custom_state;
	ListCell *cell;

	if (state->fallback_state != NULL)
		ExecEndNode(state->fallback_state);
	foreach(cell, state->filters)
	{
		BloomPGExecFilter *exec_filter = lfirst(cell);

		bloompg_runtime_release_filter(exec_filter->token);
	}
	state->parallel_store = NULL;
}

static void
bloompg_rescan_filter(CustomScanState *custom_state)
{
	BloomPGFilterState *state = (BloomPGFilterState *) custom_state;

	if (state->fallback_state == NULL)
		return;
	state->rescans++;

	/* ExecReScan() only propagates chgParam through Plan.lefttree/righttree.
	 * A CustomScan owns its children in custom_ps, so the provider must forward
	 * changed nested-loop parameters explicitly.  Leave a changed child for
	 * ExecProcNode() to rescan lazily, matching core executor convention. */
	if (custom_state->ss.ps.chgParam != NULL)
	{
		UpdateChangedParamSet(state->fallback_state,
			custom_state->ss.ps.chgParam);
		if (state->fallback_state->chgParam != NULL)
			state->parameter_rescans++;
	}
	if (state->fallback_state->chgParam == NULL)
		ExecReScan(state->fallback_state);
}

static const char *
bloompg_filter_child_name(Plan *plan)
{
	if (plan == NULL)
		return "None";
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			return "Seq Scan";
		case T_SampleScan:
			return "Sample Scan";
		case T_IndexScan:
			return "Index Scan";
		case T_IndexOnlyScan:
			return "Index Only Scan";
		case T_BitmapHeapScan:
			return "Bitmap Heap Scan";
		case T_TidScan:
			return "TID Scan";
		case T_TidRangeScan:
			return "TID Range Scan";
		case T_ForeignScan:
			return "Foreign Scan";
		case T_CustomScan:
			return "Custom Scan";
		case T_Append:
			return "Append";
		case T_MergeAppend:
			return "Merge Append";
		case T_SubqueryScan:
			return "Subquery Scan";
		default:
			return "Other";
	}
}

static void
bloompg_explain_filter(CustomScanState *custom_state,
						   List *ancestors,
						   ExplainState *explain)
{
	BloomPGFilterState *state = (BloomPGFilterState *) custom_state;
	int active = 0;
	ListCell *cell;

	(void) ancestors;
	foreach(cell, state->filters)
	{
		BloomPGExecFilter *exec_filter = lfirst(cell);

		if (exec_filter->filter != NULL)
			active++;
	}
	ExplainPropertyText("Wrapped Access Path",
		bloompg_filter_child_name(state->fallback_state != NULL ?
			state->fallback_state->plan : NULL), explain);
	ExplainPropertyBool("Parameterized Child",
		state->fallback_state != NULL &&
		state->fallback_state->plan->extParam != NULL, explain);
	ExplainPropertyInteger("Direct Filters", NULL,
		(int64) list_length(state->filters), explain);
	ExplainPropertyInteger("Active Filters", NULL, (int64) active, explain);
	ExplainPropertyBool("Parallel Filter DSM", state->parallel_active, explain);
	if (explain->analyze)
	{
		ExplainPropertyInteger("Rescans", NULL, (int64) state->rescans,
			explain);
		ExplainPropertyInteger("Parameter Change Rescans", NULL,
			(int64) state->parameter_rescans, explain);
		ExplainPropertyInteger(state->parallel_active ?
			"Leader Rows Rejected" : "Rows Rejected", NULL,
			(int64) state->rejected, explain);
	}
}
