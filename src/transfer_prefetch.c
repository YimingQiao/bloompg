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

/* Physical pair prefetch policy; logical transfer ordering stays in transfer.c. */

static void
bloompg_record_prefetch(BloomPGRelationState *state, uint64 token,
	BloomPGColumnStore *store, TupleDesc descriptor, uint64 rows,
	double scan_ms, int workers_requested, int workers_launched)
{
	bloompg_runtime_register_materialization(token, GetActiveSnapshot(), store,
		descriptor, rows);
	state->prefetched = true;
	state->prefetch_token = token;
	state->prefetch_rows = rows;
	state->materialization_used_filters |= state->filters != NIL;
	/* The detached parallel plan consumed all Apply nodes that existed when
	 * the speculative scan started.  Filters installed by its batch peer after
	 * this point are appended to a fresh list and applied during compaction. */
	state->filters = NIL;
	state->exact_scans++;
	state->exact_scan_ms += scan_ms;
	state->base_materializations++;
	state->base_materialize_ms += scan_ms;
	state->parallel_scans++;
	state->parallel_workers_requested += workers_requested;
	state->parallel_workers_launched += workers_launched;
	state->parallel_rows += rows;
	state->parallel_scan_ms += scan_ms;
	state->materialization_access_path = "parallel-prefetch-p0";
}

static void
bloompg_record_parallel_wave(BloomPGRelationState *state, uint64 wave_id,
	const char *kind, const char *layout, bool coordinator, Index peer_rti,
	double wave_ms,
	int workers_requested, int workers_launched)
{
	Assert(wave_id != 0 && kind != NULL && layout != NULL && peer_rti != 0);
	state->parallel_wave_id = wave_id;
	state->parallel_wave_kind = kind;
	state->parallel_wave_layout = layout;
	state->parallel_wave_coordinator = coordinator;
	state->parallel_wave_peer_rti = peer_rti;
	state->parallel_wave_ms = wave_ms;
	state->parallel_wave_workers_requested = workers_requested;
	state->parallel_wave_workers_launched = workers_launched;
}

static bool
bloompg_prefetch_edge_matches(const BloomPGJoinEdge *edge, Index source_rti,
	Index destination_rti)
{
	if (edge->left.rti == source_rti && edge->right.rti == destination_rti)
		return !edge->protect_right;
	if (edge->right.rti == source_rti && edge->left.rti == destination_rti)
		return !edge->protect_left;
	return false;
}

static bool
bloompg_prefetch_would_lose_index(BloomPGRelationState *source,
	BloomPGRelationState *peer, BloomPGRelationState **states, int count,
	const BloomPGJoinGraph *graph)
{
	ListCell *cell;

	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *edge = lfirst(cell);
		BloomPGRelationState *producer = NULL;
		BloomPGColumnRef probe;

		if (edge->left.rti == peer->rti)
		{
			probe = edge->left;
			producer = edge->right.rti <= (Index) count ?
				states[edge->right.rti] : NULL;
		}
		else if (edge->right.rti == peer->rti)
		{
			probe = edge->right;
			producer = edge->left.rti <= (Index) count ?
				states[edge->left.rti] : NULL;
		}
		else
			continue;
		if (producer == NULL || producer == peer ||
			(producer != source && !producer->active) ||
			producer->current_rows > (double) bloompg_index_transfer_max_keys ||
			!bloompg_prefetch_edge_matches(edge, producer->rti, peer->rti) ||
			!bloompg_relation_has_transfer_index(peer, probe.attribute,
				edge->operator_oid, edge->input_collation))
			continue;
		return true;
	}
	return false;
}

static BloomPGRelationState *
bloompg_select_prefetch_peer(BloomPGRelationState *source,
	BloomPGRelationState **states, int count, const BloomPGJoinGraph *graph,
	bool query_parallel_safe)
{
	BloomPGRelationState *best = NULL;
	int rti;

	for (rti = 1; rti <= count; rti++)
	{
		BloomPGRelationState *candidate = states[rti];
		int *mapping = NULL;
		int columns = 0;
		bool compact_layout;

		if (candidate == NULL || candidate == source || !candidate->active ||
			candidate->materialized || candidate->prefetched ||
			bloompg_transfer_worker_count(candidate, query_parallel_safe) <= 0)
			continue;
		if (candidate->join_index_sources != NULL &&
			candidate->base_rows > (double) bloompg_index_guard_min_rows)
			continue;
		/* A shared heap wave is irreversible: it materializes the peer before
		 * the source can publish its exact keys.  Do not trade away a B-tree
		 * access opportunity merely to overlap two scans. */
		if (bloompg_prefetch_would_lose_index(source, candidate, states,
			count, graph))
			continue;
		/* Preserve the smallest-active schedule that makes DuckDB Bloom avoid
		 * premature large materializations.  Unrelated sources may share a wave
		 * only when they are adjacent in scale; lineage-related peers retain the
		 * original safe speculative rule. */
		if (!bms_overlap(source->lineage, candidate->lineage) &&
			candidate->current_rows > Max(source->current_rows, 1.0) * 2.0)
			continue;
		compact_layout = bloompg_batch_targetlists_permutable(
			source->fallback_plan, candidate->fallback_plan, &mapping, &columns);
		if (mapping != NULL)
			pfree(mapping);
		if (!compact_layout &&
			Max(source->current_rows, candidate->current_rows) > 250000.0)
			continue;
		/* Only heap reads overlap.  A heterogeneous batch retains both native
		 * layouts, and the peer is compacted with newly produced filters before
		 * it may build an outgoing filter. */
		if (best == NULL || candidate->current_rows < best->current_rows ||
			(candidate->current_rows == best->current_rows &&
			 candidate->rti < best->rti))
			best = candidate;
	}
	return best;
}

/*
 * Speculatively materialize the next two excited base sources in one physical
 * wave.  Logical propagation remains ordered: a filter produced by the first
 * source is applied to the peer's already-columnar result before that peer is
 * allowed to propagate.  Thus this changes scheduling, not the transfer fixed
 * point, and each heap is still read at most once.
 */
bool
bloompg_prefetch_active_pair(BloomPGRelationState *source,
	BloomPGRelationState **states, int relation_count,
	const BloomPGJoinGraph *graph, PlannedStmt *candidate,
	const char *query_string, ParamListInfo bound_params,
	bool query_parallel_safe,
	int *next_plan_node_id, uint64 *next_parallel_wave_id)
{
	BloomPGRelationState *peer;
	BloomPGColumnStore *source_store;
	BloomPGColumnStore *peer_store;
	TupleDesc source_descriptor = NULL;
	TupleDesc peer_descriptor = NULL;
	uint64 source_token;
	uint64 peer_token;
	uint64 source_rows = 0;
	uint64 peer_rows = 0;
	int source_requested;
	int peer_requested;
	int available_workers;
	double source_local_ms = 0.0;
	double source_filter_ms = 0.0;
	double peer_local_ms = 0.0;
	double peer_filter_ms = 0.0;
	double source_score;
	double peer_score;
	int worker_budget;
	bool shared_batch = false;
	int shared_workers = 0;
	int shared_launched = 0;
	Size parallel_memory = 0;
	double shared_ms = 0.0;
	const char *shared_layout = "unknown";
	uint64 wave_id;

	if (source->materialized || source->prefetched ||
		(source->join_index_sources != NULL &&
		 source->base_rows > (double) bloompg_index_guard_min_rows))
		return false;
	source_requested = bloompg_transfer_worker_count(source,
		query_parallel_safe);
	if (source_requested <= 0)
		return false;
	peer = bloompg_select_prefetch_peer(source, states, relation_count, graph,
		query_parallel_safe);
	if (peer == NULL)
		return false;
	peer_requested = bloompg_transfer_worker_count(peer, query_parallel_safe);
	available_workers = Max(max_parallel_workers, 0);
	if (available_workers < 2)
		return false;
	/* Keep both groups within PostgreSQL's global worker budget.  Two medium
	 * heaps share one Parallel Append group, so cap their combined request at
	 * 64 rather than allowing a machine-wide 128-process fan-out.  Within that
	 * budget, use measured sample
	 * expression/filter cost to give three quarters to a clearly dearer scan. */
	worker_budget = Min(available_workers, source_requested + peer_requested);
	if (source->base_rows < 5000000.0 && peer->base_rows < 5000000.0)
		worker_budget = Min(worker_budget, 64);
	(void) bloompg_sample_workspace_order_costs(source->sample_workspace,
		&source_local_ms, &source_filter_ms);
	(void) bloompg_sample_workspace_order_costs(peer->sample_workspace,
		&peer_local_ms, &peer_filter_ms);
	source_score = source_local_ms + source_filter_ms;
	peer_score = peer_local_ms + peer_filter_ms;
	if (source_score > peer_score * 2.0)
	{
		source_requested = Min(source_requested,
			Max(1, (worker_budget * 3) / 4));
		peer_requested = Min(peer_requested,
			Max(1, worker_budget - source_requested));
	}
	else if (peer_score > source_score * 2.0)
	{
		peer_requested = Min(peer_requested,
			Max(1, (worker_budget * 3) / 4));
		source_requested = Min(source_requested,
			Max(1, worker_budget - peer_requested));
	}
	else
	{
		source_requested = Min(source_requested, worker_budget / 2);
		peer_requested = Min(peer_requested,
			worker_budget - source_requested);
	}
	if (source_requested <= 0 || peer_requested <= 0)
		return false;

	shared_workers = Min(available_workers,
		source_requested + peer_requested);
	source_token = bloompg_runtime_next_token();
	peer_token = bloompg_runtime_next_token();
	source_store = bloompg_begin_runtime_store(source->memory_budget);
	peer_store = bloompg_begin_runtime_store(peer->memory_budget);
	bloompg_runtime_register_materialization(source_token, GetActiveSnapshot(),
		source_store, NULL, 0);
	bloompg_runtime_register_materialization(peer_token, GetActiveSnapshot(),
		peer_store, NULL, 0);
	parallel_memory = bloompg_parallel_pack_memory(shared_workers);
	if (!bloompg_memory_budget_reserve(source->memory_budget, parallel_memory))
	{
		bloompg_runtime_release_materialization(source_token);
		bloompg_runtime_release_materialization(peer_token);
		return false;
	}

	PG_TRY();
	{
		shared_batch = bloompg_execute_parallel_batch(source, peer, candidate,
			source_store, peer_store, query_string, bound_params,
			shared_workers, next_plan_node_id, &source_rows, &peer_rows,
			&source_descriptor, &peer_descriptor, &shared_launched,
			&shared_ms, &shared_layout);
	}
	PG_CATCH();
	{
		bloompg_memory_budget_release_reservation(source->memory_budget,
			parallel_memory);
		bloompg_runtime_release_materialization(source_token);
		bloompg_runtime_release_materialization(peer_token);
		PG_RE_THROW();
	}
	PG_END_TRY();
	bloompg_memory_budget_release_reservation(source->memory_budget,
		parallel_memory);
	if (!shared_batch)
	{
		bloompg_runtime_release_materialization(source_token);
		bloompg_runtime_release_materialization(peer_token);
		return false;
	}
	wave_id = (*next_parallel_wave_id)++;
	bloompg_record_prefetch(source, source_token, source_store,
		source_descriptor, source_rows, shared_ms, shared_workers,
		shared_launched);
	bloompg_record_prefetch(peer, peer_token, peer_store, peer_descriptor,
		peer_rows, shared_ms, 0, 0);
	bloompg_record_parallel_wave(source, wave_id, "parallel-append",
		shared_layout, true,
		peer->rti, shared_ms, shared_workers, shared_launched);
	bloompg_record_parallel_wave(peer, wave_id, "parallel-append",
		shared_layout, false,
		source->rti, shared_ms, shared_workers, shared_launched);
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg parallel wave " UINT64_FORMAT
			" kind=%s layout=%s relations=%u,%u rows=" UINT64_FORMAT "," UINT64_FORMAT
			" workers=%d/%d wall=%.3fms",
			wave_id, "parallel-append", shared_layout,
			source->rti, peer->rti, source_rows, peer_rows,
			shared_launched, shared_workers, shared_ms);
	return true;
}
