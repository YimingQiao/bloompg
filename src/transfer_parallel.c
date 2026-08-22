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
#include "optimizer/clauses.h"
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

/* PostgreSQL worker-plan construction and shared Parallel Append waves. */

typedef struct BloomPGBatchReceiver
{
	DestReceiver receiver;
	BloomPGColumnStore *stores[2];
	TupleDesc	descriptors[2];
	uint64		rows_seen[2];
	bool		owns_descriptors;
} BloomPGBatchReceiver;

static bool
bloompg_receive_batch_slot(TupleTableSlot *slot, DestReceiver *receiver)
{
	BloomPGBatchReceiver *batch = (BloomPGBatchReceiver *) receiver;
	bool source_is_null;
	bool packet_is_null;
	Datum source_value = slot_getattr(slot, 1, &source_is_null);
	Datum packet;
	int source;

	if (source_is_null)
		elog(ERROR, "bloompg parallel batch returned a NULL source id");
	source = DatumGetInt32(source_value);
	if (source < 0 || source > 1)
		elog(ERROR, "bloompg parallel batch returned source id %d", source);
	packet = slot_getattr(slot, 2, &packet_is_null);
	if (packet_is_null)
		elog(ERROR, "bloompg parallel batch returned a NULL vector packet");
	batch->rows_seen[source] += bloompg_receive_packed_chunk(packet,
		batch->stores[source], batch->descriptors[source], NULL, NULL);
	return true;
}

static void
bloompg_batch_receiver_startup(DestReceiver *receiver, int operation,
	TupleDesc descriptor)
{
	(void) receiver;
	(void) operation;
	if (descriptor->natts != 2 ||
		TupleDescAttr(descriptor, 0)->atttypid != INT4OID ||
		TupleDescAttr(descriptor, 1)->atttypid != BYTEAOID)
		elog(ERROR, "invalid bloompg parallel batch packet descriptor");
}

static void
bloompg_batch_receiver_shutdown(DestReceiver *receiver)
{
	(void) receiver;
}

static void
bloompg_batch_receiver_destroy(DestReceiver *receiver)
{
	BloomPGBatchReceiver *batch = (BloomPGBatchReceiver *) receiver;

	if (batch->owns_descriptors)
	{
		for (int source = 0; source < 2; source++)
		{
			if (batch->descriptors[source] != NULL)
				FreeTupleDesc(batch->descriptors[source]);
		}
	}
	pfree(receiver);
}

/* Each Parallel Append branch packs its native projection before the common
 * source tag is added.  Both branches consequently expose the same compact
 * (int4, bytea) transport descriptor even when their payload layouts differ. */
static BloomPGBatchReceiver *
bloompg_create_batch_receiver(Plan *source_plan, Plan *peer_plan,
	BloomPGColumnStore *source_store, BloomPGColumnStore *peer_store)
{
	BloomPGBatchReceiver *batch = palloc0(sizeof(BloomPGBatchReceiver));
	MemoryContext old_context;

	batch->receiver.receiveSlot = bloompg_receive_batch_slot;
	batch->receiver.rStartup = bloompg_batch_receiver_startup;
	batch->receiver.rShutdown = bloompg_batch_receiver_shutdown;
	batch->receiver.rDestroy = bloompg_batch_receiver_destroy;
	batch->receiver.mydest = DestNone;
	batch->stores[0] = source_store;
	batch->stores[1] = peer_store;
	old_context = MemoryContextSwitchTo(TopTransactionContext);
	batch->owns_descriptors = true;
	PG_TRY();
	{
		batch->descriptors[0] = ExecCleanTypeFromTL(source_plan->targetlist);
		batch->descriptors[1] = ExecCleanTypeFromTL(peer_plan->targetlist);
		for (int source = 0; source < 2; source++)
		{
			bloompg_column_store_set_descriptor(batch->stores[source],
				batch->descriptors[source]);
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(old_context);
		bloompg_batch_receiver_destroy((DestReceiver *) batch);
		PG_RE_THROW();
	}
	PG_END_TRY();
	MemoryContextSwitchTo(old_context);
	return batch;
}

static bool
bloompg_transfer_filters_parallel_safe(List *filters)
{
	ListCell *cell;

	foreach(cell, filters)
	{
		BloomPGSampleFilter *sample_filter = lfirst(cell);
		BloomPGFilter *filter = sample_filter->filter;
		int key;

		if (filter == NULL || !filter->valid ||
			sample_filter->attributes == NULL)
			return false;
		for (key = 0; key < filter->nkeys; key++)
		{
			if (!AttributeNumberIsValid(sample_filter->attributes[key]) ||
				!OidIsValid(filter->keys[key].probe_hash_proc) ||
				func_parallel(filter->keys[key].probe_hash_proc) !=
					PROPARALLEL_SAFE)
				return false;
		}
	}
	return true;
}

static List *
bloompg_parallel_filter_metadata(List *filters)
{
	List *metadata = NIL;
	ListCell *cell;

	foreach(cell, filters)
	{
		BloomPGSampleFilter *filter = lfirst(cell);
		List *attributes = NIL;
		int key;

		for (key = 0; key < filter->nkeys; key++)
			attributes = lappend_int(attributes,
				filter->attributes[key]);
		metadata = lappend(metadata, list_make3(
			makeString(psprintf(UINT64_FORMAT, filter->token)),
			makeInteger(filter->nkeys), attributes));
	}
	return metadata;
}

/* P0 is intentionally planned without CURSOR_OPT_PARALLEL_OK, so its scan
 * flags cannot distinguish a locally safe relation from a query made
 * restricted by a SubLink elsewhere.  Recheck exactly the expressions that
 * the detached SeqScan will execute. */
static bool
bloompg_transfer_source_parallel_safe(BloomPGRelationState *state)
{
	Plan *source = state->fallback_plan;
	Query *probe;

	if (!bloompg_scan_has_base_slot_semantics(source) ||
		source->initPlan != NIL || !bms_is_empty(source->extParam))
		return false;
	probe = makeNode(Query);
	probe->commandType = CMD_SELECT;
	probe->querySource = QSRC_ORIGINAL;
	probe->canSetTag = false;
	probe->targetList = source->targetlist;
	probe->jointree = makeFromExpr(NIL,
		(Node *) bloompg_scan_base_quals(source));
	return max_parallel_hazard(probe) == PROPARALLEL_SAFE;
}

static List *
bloompg_make_outer_targetlist(List *child_targetlist)
{
	List *result = NIL;
	ListCell *cell;

	foreach(cell, child_targetlist)
	{
		TargetEntry *source = lfirst_node(TargetEntry, cell);
		TargetEntry *entry = copyObject(source);

		entry->expr = (Expr *) makeVarFromTargetEntry(OUTER_VAR, source);
		result = lappend(result, entry);
	}
	return result;
}

static Plan *
bloompg_make_parallel_gather(Plan *child, int workers, int plan_node_id)
{
	Gather *gather = makeNode(Gather);
	Plan *plan = &gather->plan;

	plan->startup_cost = child->startup_cost + parallel_setup_cost;
	plan->total_cost = child->total_cost + parallel_setup_cost +
		child->plan_rows * parallel_tuple_cost;
	plan->plan_rows = child->plan_rows;
	plan->plan_width = child->plan_width;
	plan->parallel_aware = false;
	plan->parallel_safe = false;
	plan->async_capable = false;
	plan->plan_node_id = plan_node_id;
	plan->targetlist = bloompg_make_outer_targetlist(child->targetlist);
	plan->qual = NIL;
	plan->lefttree = child;
	plan->righttree = NULL;
	plan->initPlan = NIL;
	plan->extParam = bms_copy(child->extParam);
	plan->allParam = bms_copy(child->allParam);
	gather->num_workers = workers;
	gather->rescan_param = -1;
	gather->single_copy = false;
	gather->invisible = false;
	gather->initParam = NULL;
	return plan;
}
int
bloompg_transfer_worker_count(BloomPGRelationState *state,
	bool query_parallel_safe)
{
	int workers;
	int row_workers;
	bool source_parallel_safe;
	static const double rows_per_worker = 100000.0;

	/* A correlated SubLink can make the whole query parallel-unsafe even when
	 * this detached base-relation scan is independently safe.  The plan-node
	 * flag is the precise property needed by the transfer executor. */
	source_parallel_safe = query_parallel_safe ||
		(state->fallback_plan != NULL && state->fallback_plan->parallel_safe) ||
		bloompg_transfer_source_parallel_safe(state);
	if (!source_parallel_safe || bloompg_transfer_workers <= 0 ||
		max_parallel_workers_per_gather <= 0 || max_parallel_workers <= 0 ||
		IsParallelWorker() || IsInParallelMode() ||
		state->table->rte_kind != RTE_RELATION ||
		get_rel_persistence(state->table->relation_oid) == RELPERSISTENCE_TEMP ||
		!bloompg_scan_has_base_slot_semantics(state->fallback_plan))
		return 0;
	if (state->base_rows < (double) bloompg_transfer_parallel_min_rows ||
		!bloompg_transfer_filters_parallel_safe(state->filters))
		return 0;
	workers = Min(bloompg_transfer_workers,
		max_parallel_workers_per_gather);
	workers = Min(workers, max_parallel_workers);
	/* PostgreSQL process startup and Gather tuple queues dominate when a small
	 * relation is given a machine-wide worker cap.  Treat the GUC as an upper
	 * bound and scale the physical group with the estimated input.  Large fact
	 * tables can still reach the configured cap, while medium-sized inputs use
	 * a proportionally smaller worker group. */
	if (state->base_rows >= rows_per_worker * (double) INT_MAX)
		row_workers = INT_MAX;
	else
	{
		row_workers = (int) (state->base_rows / rows_per_worker);
		if ((double) row_workers * rows_per_worker < state->base_rows)
			row_workers++;
		row_workers = Max(row_workers, 2);
	}
	workers = Min(workers, row_workers);
	return Max(workers, 0);
}

static Plan *
bloompg_make_parallel_child(BloomPGRelationState *state,
	int *next_plan_node_id)
{
	SeqScan *scan = bloompg_make_seqscan_evaluator(state->fallback_plan);
	Plan *child;

	if (scan == NULL)
		elog(ERROR, "bloompg cannot normalize the parallel transfer scan");
	child = (Plan *) scan;
	child->parallel_aware = true;
	child->parallel_safe = true;
	child->async_capable = false;
	if (state->filters != NIL)
	{
		List *metadata = bloompg_parallel_filter_metadata(state->filters);

		child = bloompg_make_filter_scan(child, metadata,
			(*next_plan_node_id)++, true, true);
	}
	return child;
}

Plan *
bloompg_make_parallel_source(BloomPGRelationState *state, int workers,
	int *next_plan_node_id)
{
	Plan *child = bloompg_make_parallel_child(state, next_plan_node_id);
	Plan *packed = bloompg_make_pack_scan(child, (*next_plan_node_id)++, workers);

	return bloompg_make_parallel_gather(packed, workers,
		(*next_plan_node_id)++);
}

bool
bloompg_batch_targetlists_permutable(Plan *source, Plan *peer,
	int **common_to_peer, int *columns)
{
	int count = list_length(source->targetlist);
	bool *used;
	int *mapping;
	int source_index;

	if (count <= 0 || count != list_length(peer->targetlist))
		return false;
	used = palloc0_array(bool, count);
	mapping = palloc_array(int, count);
	for (source_index = 0; source_index < count; source_index++)
	{
		TargetEntry *source_entry = list_nth_node(TargetEntry,
			source->targetlist, source_index);
		int peer_index;

		if (source_entry->resjunk)
		{
			pfree(used);
			pfree(mapping);
			return false;
		}
		for (peer_index = 0; peer_index < count; peer_index++)
		{
			TargetEntry *peer_entry;

			if (used[peer_index])
				continue;
			peer_entry = list_nth_node(TargetEntry, peer->targetlist,
				peer_index);
			if (!peer_entry->resjunk &&
				exprType((Node *) source_entry->expr) ==
					exprType((Node *) peer_entry->expr) &&
				exprTypmod((Node *) source_entry->expr) ==
					exprTypmod((Node *) peer_entry->expr) &&
				exprCollation((Node *) source_entry->expr) ==
					exprCollation((Node *) peer_entry->expr))
			{
				mapping[source_index] = peer_index;
				used[peer_index] = true;
				break;
			}
		}
		if (peer_index == count)
		{
			pfree(used);
			pfree(mapping);
			return false;
		}
	}
	pfree(used);
	*common_to_peer = mapping;
	*columns = count;
	return true;
}

static Plan *
bloompg_make_batch_packet_result(Plan *child, int source_id,
	int plan_node_id)
{
	Result *result = makeNode(Result);
	Plan *plan = &result->plan;
	List *targetlist = NIL;
	TargetEntry *packet_entry;

	targetlist = lappend(targetlist,
		makeTargetEntry((Expr *) makeConst(INT4OID, -1, InvalidOid,
			(int16) sizeof(int32), Int32GetDatum(source_id), false, true),
			1, pstrdup("__bloompg_batch_source"), false));
	packet_entry = linitial_node(TargetEntry, child->targetlist);
	targetlist = lappend(targetlist,
		makeTargetEntry((Expr *) makeVarFromTargetEntry(OUTER_VAR, packet_entry),
			2, pstrdup("__bloompg_vector_packet"), false));
	plan->startup_cost = child->startup_cost;
	plan->total_cost = child->total_cost;
	plan->plan_rows = child->plan_rows;
	plan->plan_width = add_size(child->plan_width, sizeof(int32));
	plan->parallel_aware = false;
	plan->parallel_safe = true;
	plan->async_capable = false;
	plan->plan_node_id = plan_node_id;
	plan->targetlist = targetlist;
	plan->qual = NIL;
	plan->lefttree = child;
	plan->righttree = NULL;
	plan->initPlan = NIL;
	plan->extParam = bms_copy(child->extParam);
	plan->allParam = bms_copy(child->allParam);
	result->resconstantqual = NULL;
	return plan;
}

static Plan *
bloompg_make_parallel_batch_plan(BloomPGRelationState *source,
	BloomPGRelationState *peer, int workers, int *next_plan_node_id)
{
	Plan *source_child = bloompg_make_parallel_child(source,
		next_plan_node_id);
	Plan *peer_child = bloompg_make_parallel_child(peer, next_plan_node_id);
	Plan *source_packed = bloompg_make_pack_scan(source_child,
		(*next_plan_node_id)++, workers);
	Plan *peer_packed = bloompg_make_pack_scan(peer_child,
		(*next_plan_node_id)++, workers);
	Plan *source_result;
	Plan *peer_result;
	Append *append = makeNode(Append);
	Plan *append_plan = &append->plan;

	source_result = bloompg_make_batch_packet_result(source_packed, 0,
		(*next_plan_node_id)++);
	peer_result = bloompg_make_batch_packet_result(peer_packed, 1,
		(*next_plan_node_id)++);

	append_plan->startup_cost = Min(source_result->startup_cost,
		peer_result->startup_cost);
	append_plan->total_cost = source_result->total_cost +
		peer_result->total_cost;
	append_plan->plan_rows = source_result->plan_rows + peer_result->plan_rows;
	append_plan->plan_width = source_result->plan_width;
	append_plan->parallel_aware = true;
	append_plan->parallel_safe = true;
	append_plan->async_capable = false;
	append_plan->plan_node_id = (*next_plan_node_id)++;
	append_plan->targetlist = copyObject(source_result->targetlist);
	append_plan->qual = NIL;
	append_plan->lefttree = NULL;
	append_plan->righttree = NULL;
	append_plan->initPlan = NIL;
	append_plan->extParam = bms_union(source_result->extParam,
		peer_result->extParam);
	append_plan->allParam = bms_union(source_result->allParam,
		peer_result->allParam);
	append->apprelids = bms_union(source->scope_relids, peer->scope_relids);
	append->apprelids = bms_add_member(append->apprelids, source->rti);
	append->apprelids = bms_add_member(append->apprelids, peer->rti);
	append->appendplans = list_make2(source_result, peer_result);
	append->nasyncplans = 0;
	append->first_partial_plan = 0;
	append->part_prune_index = -1;
	return bloompg_make_parallel_gather(append_plan, workers,
		(*next_plan_node_id)++);
}

bool
bloompg_execute_parallel_batch(BloomPGRelationState *source,
	BloomPGRelationState *peer, PlannedStmt *candidate,
	BloomPGColumnStore *source_store, BloomPGColumnStore *peer_store,
	const char *query_string, ParamListInfo bound_params, int workers,
	int *next_plan_node_id, uint64 *source_rows, uint64 *peer_rows,
	TupleDesc *source_descriptor, TupleDesc *peer_descriptor,
	int *workers_launched, double *batch_ms, const char **batch_layout)
{
	PlannedStmt *detached;
	BloomPGBatchReceiver *receiver;
	QueryDesc *query_desc = NULL;
	int *common_to_peer = NULL;
	int columns = 0;
	instr_time started;

	if (bloompg_batch_targetlists_permutable(source->fallback_plan,
		peer->fallback_plan, &common_to_peer, &columns))
		*batch_layout = "compact";
	else
		*batch_layout = "heterogeneous";
	detached = copyObject(candidate);
	detached->planTree = bloompg_make_parallel_batch_plan(source, peer,
		workers, next_plan_node_id);
	detached->canSetTag = false;
	detached->parallelModeNeeded = true;
	detached->rowMarks = NIL;
	detached->resultRelations = NIL;
	detached->jitFlags = 0;
	receiver = bloompg_create_batch_receiver(source->fallback_plan,
		peer->fallback_plan, source_store, peer_store);
	if (common_to_peer != NULL)
		pfree(common_to_peer);
	INSTR_TIME_SET_CURRENT(started);
	PG_TRY();
	{
		query_desc = CreateQueryDesc(detached,
			query_string != NULL ? query_string : "<bloompg parallel batch>",
			GetActiveSnapshot(), InvalidSnapshot, (DestReceiver *) receiver,
			bound_params, NULL, 0);
		ExecutorStart(query_desc, 0);
		ExecutorRun(query_desc, ForwardScanDirection, 0);
		*workers_launched = castNode(GatherState,
			query_desc->planstate)->nworkers_launched;
		ExecutorFinish(query_desc);
		ExecutorEnd(query_desc);
		FreeQueryDesc(query_desc);
		query_desc = NULL;
		*source_rows = receiver->rows_seen[0];
		*peer_rows = receiver->rows_seen[1];
		*source_descriptor = receiver->descriptors[0];
		*peer_descriptor = receiver->descriptors[1];
		receiver->owns_descriptors = false;
		*batch_ms = bloompg_transfer_elapsed_ms(started);
		receiver->receiver.rDestroy((DestReceiver *) receiver);
	}
	PG_CATCH();
	{
		/* The optimizer subtransaction abort owns failed executor cleanup. */
		PG_RE_THROW();
	}
	PG_END_TRY();
	return true;
}
