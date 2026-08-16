#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/scan.h"
#include "bloompg/filter.h"
#include "bloompg/graph.h"
#include "bloompg/runtime.h"
#include "bloompg/sampling.h"
#include "bloompg/scan_quals.h"
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

/* Exact detached scans, local prefiltering, and row-to-column reception. */

typedef struct BloomPGFilterReceiver
{
	DestReceiver receiver;
	BloomPGColumnStore *store;
	TupleDesc	descriptor;
	uint64		rows_seen;
} BloomPGFilterReceiver;
typedef struct BloomPGPrefilterContext
{
	BloomPGRelationState *state;
	SeqScanState *scan_state;
	BloomPGFilterReceiver *receiver;
	BloomPGSampleFilter **filters;
	int			filter_count;
	bool		local_first;
} BloomPGPrefilterContext;

static bool
bloompg_receive_filter_slot(TupleTableSlot *slot, DestReceiver *receiver)
{
	BloomPGFilterReceiver *filter_receiver = (BloomPGFilterReceiver *) receiver;

	filter_receiver->rows_seen++;
	bloompg_column_store_append_slot(filter_receiver->store, slot);
	return true;
}

static void
bloompg_receiver_startup(DestReceiver *receiver, int operation, TupleDesc descriptor)
{
	BloomPGFilterReceiver *filter_receiver = (BloomPGFilterReceiver *) receiver;
	MemoryContext old_context;

	(void) operation;
	old_context = MemoryContextSwitchTo(TopTransactionContext);
	filter_receiver->descriptor = CreateTupleDescCopy(descriptor);
	MemoryContextSwitchTo(old_context);
	bloompg_column_store_set_descriptor(filter_receiver->store,
		filter_receiver->descriptor);
}

static void
bloompg_receiver_shutdown(DestReceiver *receiver)
{
	(void) receiver;
}

static void
bloompg_receiver_destroy(DestReceiver *receiver)
{
	pfree(receiver);
}

static BloomPGFilterReceiver *
bloompg_create_filter_receiver(List *pending, BloomPGColumnStore *store)
{
	BloomPGFilterReceiver *receiver = palloc0(sizeof(BloomPGFilterReceiver));

	(void) pending;

	receiver->receiver.receiveSlot = bloompg_receive_filter_slot;
	receiver->receiver.rStartup = bloompg_receiver_startup;
	receiver->receiver.rShutdown = bloompg_receiver_shutdown;
	receiver->receiver.rDestroy = bloompg_receiver_destroy;
	receiver->receiver.mydest = DestNone;
	receiver->store = store;
	return receiver;
}

static uint64
bloompg_run_source_statement(PlannedStmt *detached, List *pending,
	BloomPGColumnStore *store, TupleDesc *descriptor,
	const char *query_string, ParamListInfo bound_params,
	int parallel_workers, int *parallel_workers_launched)
{
	BloomPGFilterReceiver *receiver;
	QueryDesc *query_desc = NULL;
	uint64 rows_seen;

	receiver = bloompg_create_filter_receiver(pending, store);

	PG_TRY();
	{
		query_desc = CreateQueryDesc(detached,
								 query_string != NULL ? query_string : "<bloompg transfer>",
								 GetActiveSnapshot(), InvalidSnapshot,
								 (DestReceiver *) receiver, bound_params, NULL, 0);
		ExecutorStart(query_desc, 0);
		ExecutorRun(query_desc, ForwardScanDirection, 0);
		if (parallel_workers > 0 && IsA(query_desc->planstate, GatherState))
			*parallel_workers_launched =
				castNode(GatherState, query_desc->planstate)->nworkers_launched;
		ExecutorFinish(query_desc);
		ExecutorEnd(query_desc);
		FreeQueryDesc(query_desc);
		query_desc = NULL;
		rows_seen = receiver->rows_seen;
		*descriptor = receiver->descriptor;
		receiver->receiver.rDestroy((DestReceiver *) receiver);
	}
	PG_CATCH();
	{
		if (query_desc != NULL)
		{
			if (query_desc->estate != NULL)
				ExecutorEnd(query_desc);
			FreeQueryDesc(query_desc);
		}
		receiver->receiver.rDestroy((DestReceiver *) receiver);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return rows_seen;
}

static uint64
bloompg_execute_source(BloomPGRelationState *state,
					   PlannedStmt *candidate,
					   Plan *source_plan,
					   List *pending,
					   BloomPGColumnStore *store,
					   TupleDesc *descriptor,
					   const char *query_string,
					   ParamListInfo bound_params,
					   int parallel_workers,
					   int *next_plan_node_id,
					   int *parallel_workers_launched)
{
	PlannedStmt *detached = copyObject(candidate);
	Plan *build_plan;

	if (parallel_workers > 0)
		build_plan = bloompg_make_parallel_source(state, parallel_workers,
			next_plan_node_id);
	else
	{
		build_plan = copyObject(source_plan);
		bloompg_disable_parallel_flags(build_plan);
	}
	detached->planTree = build_plan;
	detached->canSetTag = false;
	detached->parallelModeNeeded = parallel_workers > 0;
	detached->rowMarks = NIL;
	detached->resultRelations = NIL;
	detached->jitFlags = 0;
	return bloompg_run_source_statement(detached, pending, store, descriptor,
		query_string, bound_params, parallel_workers,
		parallel_workers_launched);
}

/*
 * Core SeqScan evaluates its local quals before an Apply wrapper can inspect
 * the tuple.  That is particularly costly for JOB predicates such as a text
 * IN-list when an incoming join-key filter rejects almost every heap row.
 * For a plain base SeqScan, drive the initialized scan directly.  Candidate
 * slots may come from an exact-key B-tree probe or the heap scan; either way,
 * probe every formal transfer filter using base attribute numbers first, then
 * evaluate the original local quals and projection only for survivors.  Heap
 * slots deform lazily, so query-pruned payload/text columns are not fetched
 * for a rejected row.
 */
static bool
bloompg_prefilter_visit_slot(TupleTableSlot *scan_slot, void *opaque)
{
	BloomPGPrefilterContext *context = opaque;
	BloomPGRelationState *state = context->state;
	SeqScanState *scan_state = context->scan_state;
	ExprContext *expression_context = scan_state->ss.ps.ps_ExprContext;
	TupleTableSlot *result_slot;
	bool		accepted = true;
	int			filter_index;

	state->prefilter_rows++;
	ResetExprContext(expression_context);
	expression_context->ecxt_scantuple = scan_slot;
	if (context->local_first && !ExecQual(scan_state->ss.ps.qual,
			expression_context))
	{
		state->local_rejected++;
		return true;
	}
	for (filter_index = 0; filter_index < context->filter_count; filter_index++)
	{
		BloomPGSampleFilter *filter = context->filters[filter_index];
		bool contains;

		if (filter->filter->kind == BLOOMPG_FILTER_BITMAP &&
			filter->nkeys == 1)
		{
			bool is_null;
			Datum value = slot_getattr(scan_slot, filter->attributes[0],
				&is_null);

			contains = bloompg_filter_contains_bitmap_value(filter->filter,
				value, is_null);
		}
		else
			contains = bloompg_filter_contains_slot(filter->filter,
				scan_slot, filter->attributes);
		if (!contains)
		{
			accepted = false;
			break;
		}
	}
	if (!accepted)
	{
		state->prefilter_rejected++;
		return true;
	}
	if (!context->local_first && scan_state->ss.ps.qual != NULL &&
		!ExecQual(scan_state->ss.ps.qual, expression_context))
	{
		state->local_rejected++;
		return true;
	}
	result_slot = scan_state->ss.ps.ps_ProjInfo != NULL ?
		ExecProject(scan_state->ss.ps.ps_ProjInfo) : scan_slot;
	return context->receiver->receiver.receiveSlot(result_slot,
		(DestReceiver *) context->receiver);
}

static uint64
bloompg_execute_prefiltered_base_scan(BloomPGRelationState *state,
									PlannedStmt *candidate,
									List *pending,
									BloomPGColumnStore *store,
									TupleDesc *descriptor,
									const char *query_string,
									ParamListInfo bound_params)
{
	PlannedStmt *detached = copyObject(candidate);
	BloomPGFilterReceiver *receiver;
	QueryDesc *query_desc = NULL;
	SeqScan *scan_plan = bloompg_make_seqscan_evaluator(state->fallback_plan);
	SeqScanState *scan_state;
	TableScanDesc scan_descriptor;
	TupleTableSlot *scan_slot;
	bool receiver_started = false;
	bool local_first;
	double local_fraction;
	double transfer_fraction;
	double local_sample_ms = 0.0;
	double filter_sample_ms = 0.0;
	double local_first_score;
	double filter_first_score;
	bool costed;
	uint64 rows_seen;
	BloomPGSampleFilter **filter_array;
	BloomPGPrefilterContext prefilter_context;
	int filter_count = list_length(state->filters);
	int filter_index;

	if (scan_plan == NULL)
		elog(ERROR, "bloompg cannot normalize the transfer base scan");
	bloompg_disable_parallel_flags((Plan *) scan_plan);
	detached->planTree = (Plan *) scan_plan;
	detached->canSetTag = false;
	detached->parallelModeNeeded = false;
	detached->rowMarks = NIL;
	detached->resultRelations = NIL;
	detached->jitFlags = 0;
	receiver = bloompg_create_filter_receiver(pending, store);
	filter_array = filter_count > 0 ?
		palloc_array(BloomPGSampleFilter *, filter_count) : NULL;
	filter_index = 0;
	foreach_ptr(BloomPGSampleFilter, filter, state->filters)
		filter_array[filter_index++] = filter;

	PG_TRY();
	{
		query_desc = CreateQueryDesc(detached,
			query_string != NULL ? query_string : "<bloompg prefiltered scan>",
			GetActiveSnapshot(), InvalidSnapshot, (DestReceiver *) receiver,
			bound_params, NULL, 0);
		ExecutorStart(query_desc, 0);
		if (!IsA(query_desc->planstate, SeqScanState))
			elog(ERROR, "bloompg prefilter expected a sequential scan state");
		scan_state = castNode(SeqScanState, query_desc->planstate);
		local_fraction = state->base_rows > 0.0 ?
			Min(Max(state->local_rows / state->base_rows, 0.0), 1.0) : 1.0;
		/* The sample separately measures local and filter-only selectivity.  A
		 * very selective local qual is not enough reason to run it first when the
		 * incoming filters also reject many rows; this distinction is crucial for
		 * expensive JOB LIKE quals. */
		transfer_fraction = Min(Max(state->filter_sample_fraction, 0.0), 1.0);
		costed = bloompg_sample_workspace_order_costs(state->sample_workspace,
			&local_sample_ms, &filter_sample_ms);
		local_first_score = local_sample_ms + local_fraction * filter_sample_ms;
		filter_first_score = filter_sample_ms + transfer_fraction * local_sample_ms;
		local_first = scan_state->ss.ps.qual != NULL && state->local_sampled &&
			local_fraction <= bloompg_local_first_fraction &&
			(costed ? local_first_score * 1.08 < filter_first_score :
			 transfer_fraction >= 0.9);
		state->prefilter_local_fraction = local_fraction;
		state->prefilter_transfer_fraction = transfer_fraction;
		state->prefilter_local_sample_ms = local_sample_ms;
		state->prefilter_filter_sample_ms = filter_sample_ms;
		state->prefilter_local_first_score = local_first_score;
		state->prefilter_filter_first_score = filter_first_score;
		state->prefilter_costed = costed;
		query_desc->estate->es_direction = ForwardScanDirection;
		receiver->receiver.rStartup((DestReceiver *) receiver,
			query_desc->operation, query_desc->tupDesc);
		receiver_started = true;

		/* An exact empty build cannot match, so it is safe to avoid touching
		 * the heap (and any local expressions) altogether. */
		for (filter_index = 0; filter_index < filter_count; filter_index++)
		{
			BloomPGSampleFilter *filter = filter_array[filter_index];

			if (filter->filter == NULL || !filter->filter->valid)
				elog(ERROR, "bloompg prefilter found an invalid transfer filter");
			if (filter->filter->inserted == 0)
				goto scan_complete;
		}
		prefilter_context.state = state;
		prefilter_context.scan_state = scan_state;
		prefilter_context.receiver = receiver;
		prefilter_context.filters = filter_array;
		prefilter_context.filter_count = filter_count;
		prefilter_context.local_first = local_first;
		if (bloompg_index_prefilter_scan(state,
				scan_state->ss.ss_currentRelation,
				query_desc->estate->es_snapshot,
				bloompg_prefilter_visit_slot, &prefilter_context))
			goto scan_complete;
		state->materialization_access_path = "sequential-prefilter";

		scan_descriptor = scan_state->ss.ss_currentScanDesc;
		if (scan_descriptor == NULL)
		{
			scan_descriptor = table_beginscan(
				scan_state->ss.ss_currentRelation, query_desc->estate->es_snapshot,
				0, NULL);
			scan_state->ss.ss_currentScanDesc = scan_descriptor;
		}
		scan_slot = scan_state->ss.ss_ScanTupleSlot;
		while (table_scan_getnextslot(scan_descriptor, ForwardScanDirection,
				scan_slot))
		{
			if (!bloompg_prefilter_visit_slot(scan_slot, &prefilter_context))
				break;
		}
		ExecClearTuple(scan_state->ss.ss_ScanTupleSlot);

scan_complete:
		receiver->receiver.rShutdown((DestReceiver *) receiver);
		receiver_started = false;
		ExecutorFinish(query_desc);
		ExecutorEnd(query_desc);
		FreeQueryDesc(query_desc);
		query_desc = NULL;
		rows_seen = receiver->rows_seen;
		*descriptor = receiver->descriptor;
		receiver->receiver.rDestroy((DestReceiver *) receiver);
	}
	PG_CATCH();
	{
		if (receiver_started)
			receiver->receiver.rShutdown((DestReceiver *) receiver);
		if (query_desc != NULL)
		{
			if (query_desc->estate != NULL)
				ExecutorEnd(query_desc);
			FreeQueryDesc(query_desc);
		}
		receiver->receiver.rDestroy((DestReceiver *) receiver);
		PG_RE_THROW();
	}
	PG_END_TRY();

	for (filter_index = 0; filter_index < filter_count; filter_index++)
	{
		BloomPGSampleFilter *filter = filter_array[filter_index];

		bloompg_runtime_release_filter(filter->token);
	}
	if (filter_array != NULL)
		pfree(filter_array);
	state->filters = NIL;
	state->prefilter_scans++;
	if (local_first)
		state->local_first_scans++;
	return rows_seen;
}

uint64
bloompg_execute_state_source(BloomPGRelationState *state,
								 PlannedStmt *candidate,
								 Plan *source_plan,
							 List *pending,
							 BloomPGColumnStore *store,
								 TupleDesc *descriptor,
								 const char *query_string,
								 ParamListInfo bound_params,
								 bool query_parallel_safe,
								 int *next_plan_node_id)
{
	instr_time started;
	instr_time elapsed;
	instr_time filter_started;
	uint64 rows;
	double scan_ms;
	double filter_ms;
	int parallel_workers = 0;
	int parallel_workers_launched = 0;
	PlannedStmt *access_statement = NULL;
	bool access_planned;

	state->materialization_used_filters |= state->filters != NIL;
	INSTR_TIME_SET_CURRENT(started);
	access_planned = state->filters != NIL &&
		!bloompg_index_bitmap_prefilter_preferred(state) &&
		bloompg_plan_transfer_access(state, query_string, bound_params,
			&access_statement);
	if (access_planned)
	{
		access_statement->rowMarks = NIL;
		access_statement->resultRelations = NIL;
		access_statement->jitFlags = 0;
		rows = bloompg_run_source_statement(access_statement, pending, store,
			descriptor, query_string, bound_params, 0,
			&parallel_workers_launched);
	}
	else if ((parallel_workers = bloompg_transfer_worker_count(state,
			 query_parallel_safe)) > 0)
	{
		state->materialization_access_path = "parallel-p0";
		rows = bloompg_execute_source(state, candidate, source_plan, pending,
			store, descriptor, query_string, bound_params, parallel_workers,
			next_plan_node_id, &parallel_workers_launched);
	}
	else if (state->table->rte_kind == RTE_RELATION &&
		state->filters != NIL &&
		(IsA(state->fallback_plan, SeqScan) ||
		 (bloompg_scan_has_base_slot_semantics(state->fallback_plan) &&
		  bloompg_index_prefilter_available(state))))
		rows = bloompg_execute_prefiltered_base_scan(state, candidate, pending,
			store, descriptor, query_string, bound_params);
	else
	{
		state->materialization_access_path = "native-p0";
		rows = bloompg_execute_source(state, candidate, source_plan, pending,
			store, descriptor, query_string, bound_params, 0,
			next_plan_node_id, &parallel_workers_launched);
	}
	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	scan_ms = INSTR_TIME_GET_MILLISEC(elapsed);
	INSTR_TIME_SET_CURRENT(filter_started);
	if (bloompg_build_pending_from_materialization(pending, store, *descriptor))
		state->vector_filter_builds++;
	else
		state->slot_filter_builds++;
	filter_ms = bloompg_transfer_elapsed_ms(filter_started);
	state->filter_prepare_ms += filter_ms;
	state->filter_build_ms += filter_ms;
	state->exact_scans++;
	state->exact_scan_ms += scan_ms + filter_ms;
	state->base_materializations++;
	state->base_materialize_ms += scan_ms;
	if (parallel_workers > 0)
	{
		state->parallel_scans++;
		state->parallel_workers_requested += parallel_workers;
		state->parallel_workers_launched += parallel_workers_launched;
		state->parallel_rows += rows;
		state->parallel_scan_ms += scan_ms;
	}
	return rows;
}
