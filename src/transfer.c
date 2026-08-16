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
#include "utils/date.h"
#include "utils/typcache.h"

static bool bloompg_edge_matches_direction(const BloomPGJoinEdge *edge,
	Index source_rti, Index destination_rti);

static bool
bloompg_is_excited(double rows, double baseline)
{
	/* Exact emptiness is always worth propagating, including when the user
	 * selects a zero threshold. */
	return rows <= 0.0 ||
		rows < Max(baseline, 1.0) * bloompg_excitation_threshold;
}

static bool
bloompg_edge_matches_direction(const BloomPGJoinEdge *edge,
							   Index source_rti, Index destination_rti)
{
	if (edge->left.rti == source_rti && edge->right.rti == destination_rti)
		return !edge->protect_right;
	if (edge->right.rti == source_rti && edge->left.rti == destination_rti)
		return !edge->protect_left;
	return false;
}

static int
bloompg_count_direction_keys(const BloomPGJoinGraph *graph,
							 Index source_rti, Index destination_rti)
{
	ListCell *cell;
	int count = 0;

	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *edge = lfirst(cell);

		if (bloompg_edge_matches_direction(edge, source_rti, destination_rti))
			count++;
	}
	return count;
}

static void
bloompg_remember_join_index_direction(BloomPGRelationState **states,
	int count, const BloomPGJoinGraph *graph, const BloomPGJoinEdge *edge,
	Index source_rti, BloomPGColumnRef probe)
{
	BloomPGRelationState *destination;

	if (source_rti > (Index) count || probe.rti > (Index) count ||
		states[source_rti] == NULL || states[probe.rti] == NULL ||
		!bloompg_edge_matches_direction(edge, source_rti, probe.rti) ||
		bloompg_count_direction_keys(graph, source_rti, probe.rti) != 1)
		return;
	destination = states[probe.rti];
	if (bloompg_relation_has_transfer_index(destination, probe.attribute,
		edge->operator_oid, edge->input_collation))
		destination->join_index_sources = bms_add_member(
			destination->join_index_sources, source_rti);
}

/* Cache static index opportunities once.  A source appears in a
 * destination's set only when a one-key exact transfer can drive the first
 * column of a usable B-tree index. */
static void
bloompg_initialize_join_index_sources(BloomPGRelationState **states,
	int count, const BloomPGJoinGraph *graph)
{
	ListCell *cell;

	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *edge = lfirst(cell);

		bloompg_remember_join_index_direction(states, count, graph, edge,
			edge->left.rti, edge->right);
		bloompg_remember_join_index_direction(states, count, graph, edge,
			edge->right.rti, edge->left);
	}
}

static bool
bloompg_filter_received_from(BloomPGRelationState *state, Index source_rti)
{
	ListCell *cell;

	foreach(cell, state->filters)
	{
		BloomPGSampleFilter *filter = lfirst(cell);

		if (bms_is_member(source_rti, filter->lineage))
			return true;
	}
	return false;
}

static bool
bloompg_has_pending_smaller_index_source(BloomPGRelationState *destination,
	BloomPGRelationState **states, int count)
{
	int member = -1;

	while ((member = bms_next_member(destination->join_index_sources,
		member)) >= 0)
	{
		BloomPGRelationState *source = member <= count ? states[member] : NULL;

		if (source != NULL && source != destination && source->active &&
			source->base_rows < destination->base_rows &&
			!bloompg_filter_received_from(destination, source->rti))
			return true;
	}
	return false;
}

static bool
bloompg_index_guard_delays(BloomPGRelationState *state,
	BloomPGRelationState **states, int count, bool *index_ready)
{
	*index_ready = false;
	if (state->materialized || state->prefetched ||
		state->join_index_sources == NULL ||
		state->base_rows <= (double) bloompg_index_guard_min_rows)
		return false;
	*index_ready = bloompg_index_prefilter_available(state);
	return !*index_ready ||
		bloompg_has_pending_smaller_index_source(state, states, count);
}

static bool
bloompg_state_precedes(BloomPGRelationState *left,
	BloomPGRelationState *right)
{
	return right == NULL || left->current_rows < right->current_rows ||
		(left->current_rows == right->current_rows && left->rti < right->rti);
}

/* Preserve the fixed point's smallest-active order, with one exception:
 * delay the first physical read of a non-small relation when an incoming
 * exact join-key set could unlock its index.  This function still identifies
 * the smallest guarded relation when every active relation waits; the caller
 * may scan it only when its estimated output is a bounded exact-key driver. */
static BloomPGRelationState *
bloompg_select_active(BloomPGRelationState **states, int count)
{
	BloomPGRelationState *best = NULL;
	BloomPGRelationState *fallback = NULL;
	int rti;

	for (rti = 1; rti <= count; rti++)
	{
		BloomPGRelationState *state = states[rti];
		bool index_ready;
		bool guarded;

		if (state == NULL || !state->active)
			continue;
		guarded = bloompg_index_guard_delays(state, states, count,
			&index_ready);
		state->selection_index_ready = index_ready;
		state->selection_guarded = guarded;
		state->selection_guard_fallback = false;
		state->selection_native_fallback = false;
		if (guarded)
		{
			if (bloompg_state_precedes(state, fallback))
				fallback = state;
		}
		else if (bloompg_state_precedes(state, best))
			best = state;
	}
	if (best != NULL)
		return best;
	if (fallback != NULL)
		fallback->selection_guard_fallback = true;
	return fallback;
}

static bool
bloompg_make_apply_spec(const BloomPGJoinGraph *graph,
						BloomPGRelationState *source,
						BloomPGRelationState *destination,
						BloomPGApplySpec *spec)
{
	ListCell *cell;
	int key_index = 0;
	bool orientation_set = false;

	MemSet(spec, 0, sizeof(*spec));
	spec->nkeys = bloompg_count_direction_keys(graph, source->rti,
											 destination->rti);
	if (spec->nkeys == 0)
		return false;
	spec->token = bloompg_runtime_next_token();
	spec->estimated_rows = (int64) Max(source->current_rows, 1.0);
	spec->operators = palloc_array(Oid, spec->nkeys);
	spec->collations = palloc_array(Oid, spec->nkeys);
	spec->build_attributes = palloc_array(AttrNumber, spec->nkeys);
	spec->probe_attributes = palloc_array(AttrNumber, spec->nkeys);
	spec->probe_base_attributes = palloc_array(AttrNumber, spec->nkeys);

	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *edge = lfirst(cell);
		bool source_is_left;
		BloomPGColumnRef build_column;
		BloomPGColumnRef probe_column;

		if (!bloompg_edge_matches_direction(edge, source->rti, destination->rti))
			continue;
		source_is_left = edge->left.rti == source->rti;
		if (!orientation_set)
		{
			spec->build_is_left = source_is_left;
			orientation_set = true;
		}
		else if (spec->build_is_left != source_is_left)
			return false;
		build_column = source_is_left ? edge->left : edge->right;
		probe_column = source_is_left ? edge->right : edge->left;
		spec->operators[key_index] = edge->operator_oid;
		spec->collations[key_index] = edge->input_collation;
		spec->build_attributes[key_index] =
			bloompg_find_state_output_attribute(source, build_column);
		spec->probe_attributes[key_index] =
			bloompg_find_state_output_attribute(destination, probe_column);
		spec->probe_base_attributes[key_index] = probe_column.attribute;
		if (!AttributeNumberIsValid(spec->build_attributes[key_index]) ||
			!AttributeNumberIsValid(spec->probe_attributes[key_index]))
			return false;
		key_index++;
	}
	return key_index == spec->nkeys;
}

/*
 * A large outer key population paired with PostgreSQL's parameterized index
 * path is the case where eager transfer is least attractive: materializing
 * the outer and then probing hundreds of thousands of keys duplicates work
 * that the native nested-loop/semi-join already performs efficiently.  If
 * the smallest initial source has no other useful destination, keep the
 * entire statement native before any optimizer-time heap scan occurs.
 */
static BloomPGRelationState *
bloompg_initial_native_index_guard(BloomPGRelationState **states,
	int relation_count, const BloomPGJoinGraph *graph)
{
	BloomPGRelationState *source = bloompg_select_active(states,
		relation_count);
	int destination_rti;
	int destinations = 0;
	int protected_destinations = 0;

	if (source == NULL ||
		source->current_rows <= (double) bloompg_index_transfer_max_keys)
		return NULL;
	for (destination_rti = 1; destination_rti <= relation_count;
		 destination_rti++)
	{
		BloomPGRelationState *destination = states[destination_rti];

		if (destination == NULL || destination == source ||
			bloompg_count_direction_keys(graph, source->rti,
				destination->rti) == 0)
			continue;
		destinations++;
		if (destination->native_parameterized_index)
			protected_destinations++;
	}
	return destinations > 0 && destinations == protected_destinations ?
		source : NULL;
}

static bool
bloompg_filter_build_specs_equal(const BloomPGApplySpec *left,
								 const BloomPGApplySpec *right)
{
	int key;

	if (left->nkeys != right->nkeys)
		return false;
	for (key = 0; key < left->nkeys; key++)
	{
		Oid left_type;
		Oid right_type;

		if (left->operators[key] != right->operators[key] ||
			left->collations[key] != right->collations[key] ||
			left->build_attributes[key] != right->build_attributes[key])
			return false;
		if (left->build_is_left != right->build_is_left)
		{
			op_input_types(left->operators[key], &left_type, &right_type);
			if (left_type != right_type)
				return false;
		}
	}
	return true;
}

static BloomPGPendingTransfer *
bloompg_find_shared_filter(List *pending_transfers,
						   const BloomPGApplySpec *spec)
{
	ListCell *cell;

	foreach(cell, pending_transfers)
	{
		BloomPGPendingTransfer *pending = lfirst(cell);

		if (bloompg_filter_build_specs_equal(&pending->spec, spec))
			return pending;
	}
	return NULL;
}


BloomPGTransferOutcome
bloompg_apply_transfers(PlannedStmt *candidate,
						Query *analysis_query,
						BloomPGJoinGraph *graph,
						const char *query_string,
						int cursor_options,
						ParamListInfo bound_params,
						ResourceOwner runtime_owner,
						bool query_parallel_safe,
						const Bitmapset *native_parameterized_indexes)
{
	BloomPGTransferOutcome outcome = {0};
	BloomPGRelationState **states;
	BloomPGEqualityDomains *equality_domains;
	Bitmapset **sent_lineage;
	Bitmapset *main_plan_relids;
	Bitmapset *used_subquery_relids = NULL;
	int relation_count = list_length(candidate->rtable);
	int matrix_width = relation_count + 1;
	int next_plan_node_id;
	uint64 next_parallel_wave_id = 1;
	ListCell *cell;
	StringInfoData detail;
	StringInfoData profile_steps;
	bool collect_profile = bloompg_profile_enabled;

	initStringInfo(&detail);
	appendStringInfo(&detail, "progress=%s",
		bloompg_transfer_progress_metric == BLOOMPG_TRANSFER_PROGRESS_NDV ?
		"ndv" : "rows");
	if (collect_profile)
		initStringInfo(&profile_steps);
	if (!ActiveSnapshotSet() || candidate->parallelModeNeeded ||
		bloompg_plan_has_parallel_node(candidate->planTree) ||
		graph->has_set_operation || graph->has_recursive_scope)
	{
		outcome.detail = detail.data;
		if (collect_profile)
			outcome.profile_json = pstrdup(
				"{\"summary\":{\"applied\":false,\"bypassed\":true},"
				"\"steps\":[],\"relations\":[]}");
		return outcome;
	}

	states = palloc0_array(BloomPGRelationState *, matrix_width);
	sent_lineage = palloc0_array(Bitmapset *, matrix_width * matrix_width);
	main_plan_relids = bloompg_plan_relids(candidate->planTree);
	foreach(cell, graph->tables)
	{
		BloomPGTable *table = lfirst(cell);
		BloomPGRelationState *state;
		Plan *scan;
		bool sampled;
		bool cache_hit;
		double sampled_rows;

		if ((table->rte_kind != RTE_RELATION &&
			 table->rte_kind != RTE_CTE &&
			 table->rte_kind != RTE_SUBQUERY) ||
			table->rti > (Index) relation_count)
			continue;
		state = palloc0(sizeof(BloomPGRelationState));
		state->rti = table->rti;
		state->replacement_rti = table->rti;
		state->table = table;
		state->analysis_query = analysis_query;
		state->cursor_options = cursor_options;
		state->replacement_kind = "unchanged";
		state->native_parameterized_index =
			bms_is_member(table->rti, native_parameterized_indexes);
		if (table->rte_kind == RTE_SUBQUERY)
		{
			int member = -1;

			if (!bloompg_map_subquery(candidate, graph, state,
				main_plan_relids, &used_subquery_relids))
			{
				appendStringInfo(&detail, " skip_subquery_map=%u", table->rti);
				continue;
			}
			state->simple_subquery = bloompg_simple_subquery(table->subquery);
			scan = bloompg_find_scope_root(candidate->planTree,
				state->scope_relids);
			if (scan == NULL)
			{
				appendStringInfo(&detail, " skip_subquery_scope=%u", table->rti);
				continue;
			}
			if (state->simple_subquery)
			{
				while ((member = bms_next_member(state->scope_relids, member)) >= 0)
				{
					if ((Index) member != state->rti)
					{
						state->replacement_rti = (Index) member;
						break;
					}
				}
			}
		}
		else
			scan = bloompg_find_scan(candidate->planTree, table->rti);
		if (scan == NULL || !bms_is_empty(scan->extParam))
		{
			appendStringInfo(&detail, " skip_scan=%u", table->rti);
			continue;
		}
		state->plan = scan;
		state->original_output_columns = list_length(scan->targetlist);
		state->materialized_output_columns = state->original_output_columns;
		if (table->rte_kind == RTE_RELATION && bloompg_late_materialize &&
			bloompg_replan_after_transfer &&
			scan->plan_width >= bloompg_late_materialize_min_width)
		{
			RangeTblEntry *rte = list_nth_node(RangeTblEntry,
				candidate->rtable, table->rti - 1);

			if (rte->relkind == RELKIND_RELATION ||
				rte->relkind == RELKIND_MATVIEW)
			{
				state->ctid_attribute = bloompg_prepare_tid_output(scan,
					table->rti, graph);
				state->tid_eligible =
					AttributeNumberIsValid(state->ctid_attribute);
				if (state->tid_eligible)
					state->materialized_output_columns =
						list_length(scan->targetlist);
				outcome.analysis_plan_modified |= state->tid_eligible;
			}
		}
		if (table->rte_kind == RTE_RELATION && bloompg_replan_after_transfer &&
			!state->tid_eligible && bloompg_prepare_projected_output(scan, table,
				graph, &state->original_output_columns,
				&state->materialized_output_columns))
			outcome.analysis_plan_modified = true;
		state->fallback_plan = copyObject(scan);
		state->base_rows = bloompg_relation_rows(candidate, table->rti, scan);
		if (table->rte_kind == RTE_RELATION)
			sampled_rows = bloompg_sample_relation(candidate, table->rti, scan,
				state->base_rows, query_string, bound_params,
				&state->sample_workspace, &state->sampling_profile,
				&sampled, &cache_hit);
		else
		{
			sampled = false;
			cache_hit = false;
			sampled_rows = scan->plan_rows;
		}
		state->current_rows = Max(sampled_rows, 0.0);
		state->local_rows = state->current_rows;
		state->filter_sample_fraction = 1.0;
		state->local_sampled = sampled;
		state->excitation_rows = state->base_rows;
		state->lineage = bms_make_singleton(table->rti);
		state->active = bloompg_is_excited(state->current_rows,
			state->excitation_rows);
		states[table->rti] = state;
		appendStringInfo(&detail, " state=%u:%.0f/%.0f%s",
			state->rti, state->current_rows, state->base_rows,
			state->active ? "*" : "");
		if (state->native_parameterized_index)
			appendStringInfoString(&detail, "(native-param-index)");
		if (sampled)
			appendStringInfo(&detail, " sample=%u:%.0f%s", table->rti,
							 state->current_rows, cache_hit ? "(cached)" : "");
	}
	bloompg_initialize_join_index_sources(states, relation_count, graph);
	equality_domains = bloompg_equality_domains_create(graph, relation_count);
	{
		BloomPGRelationState *guard_source =
			bloompg_initial_native_index_guard(states, relation_count, graph);

		if (guard_source != NULL)
		{
			int rti;

			for (rti = 1; rti <= relation_count; rti++)
			{
				if (states[rti] != NULL)
					states[rti]->active = false;
			}
			guard_source->native_index_guarded = true;
			appendStringInfo(&detail,
				" native_index_bypass=%u:%.0f>%d",
				guard_source->rti, guard_source->current_rows,
				bloompg_index_transfer_max_keys);
		}
	}

	next_plan_node_id = bloompg_max_plan_node_id(candidate->planTree) + 1;
	for (;;)
	{
		BloomPGRelationState *source = bloompg_select_active(states,
			relation_count);
		List *pending_transfers = NIL;
		BloomPGColumnStore *store;
		TupleDesc descriptor = NULL;
		uint64 materialization_token;
		uint64 rows_seen;
		Plan *memory_plan;
		bool source_replaced = false;
		bool excited;
		int destination_rti;
		instr_time source_started;

		if (source == NULL)
			break;
		/* A guarded source with at most this many rows is a safe small-driver
		 * escape hatch: its distinct key set cannot exceed the exact-key probe
		 * limit.  Above that bound, scanning it would silently turn an index
		 * scheduling heuristic into an unbounded optimizer-time heap scan.  Keep
		 * the already-built native preview instead. */
		if (source->selection_guard_fallback &&
			!source->selection_index_ready &&
			source->current_rows > (double) bloompg_index_transfer_max_keys &&
			bloompg_transfer_worker_count(source, query_parallel_safe) <= 0)
		{
			source->selection_guard_fallback = false;
			source->selection_native_fallback = true;
			outcome.applied = false;
			outcome.native_fallback = true;
			outcome.native_fallback_reason = "index-guard-stalemate";
			appendStringInfo(&detail,
				" native_fallback=%s:rti=%u:rows=%.0f>keys=%d",
				outcome.native_fallback_reason, source->rti,
				source->current_rows, bloompg_index_transfer_max_keys);
			break;
		}
		appendStringInfo(&detail,
			" select=%u:index_ready=%s:guarded=%s:fallback=%s",
			source->rti, source->selection_index_ready ? "yes" : "no",
			source->selection_guarded ? "yes" : "no",
			source->selection_guard_fallback ? "yes" : "no");
		source->active = false;
		for (destination_rti = 1; destination_rti <= relation_count;
			 destination_rti++)
		{
			BloomPGRelationState *destination = states[destination_rti];
			Bitmapset **sent = &sent_lineage[source->rti * matrix_width +
												 destination_rti];
			BloomPGPendingTransfer *pending;
			BloomPGPendingTransfer *shared;

			if (destination == NULL || destination == source ||
				bms_is_subset(source->lineage, *sent))
				continue;

			pending = palloc0(sizeof(BloomPGPendingTransfer));
			pending->destination = destination;
			pending->sent_lineage = sent;
			if (!bloompg_make_apply_spec(graph, source, destination,
											&pending->spec))
			{
				pfree(pending);
				continue;
			}
			pending->equality_domain = bloompg_equality_domain_for_transfer(
				equality_domains, source->rti, destination->rti,
				pending->spec.nkeys);
			shared = bloompg_find_shared_filter(pending_transfers, &pending->spec);
			if (shared != NULL)
			{
				shared->shared_filter = true;
				pending->shared_filter = true;
				pending->build_filter = false;
				pending->filter = bloompg_filter_retain(shared->filter);
			}
			else
			{
				pending->build_filter = true;
				pending->filter = bloompg_filter_create_bloom(CurrentMemoryContext,
					pending->spec.estimated_rows, pending->spec.nkeys,
					pending->spec.operators, pending->spec.collations,
					pending->spec.build_is_left);
			}
			if (pending->spec.nkeys == 1)
			{
				BloomPGFilterKey *key = &pending->filter->keys[0];
				TypeCacheEntry *type_cache = NULL;
				int64 ignored;

				/* A raw-value bitmap is exact only for the type's canonical
				 * same-type equality.  A user-defined hashjoinable operator may
				 * consider distinct integer representations equal; using a numeric
				 * bitmap for it could otherwise introduce false negatives. */
				if (key->build_type == key->probe_type)
					type_cache = lookup_type_cache(key->build_type,
						TYPECACHE_EQ_OPR);
				pending->integral_key = type_cache != NULL &&
					OidIsValid(type_cache->eq_opr) &&
					type_cache->eq_opr == pending->spec.operators[0] &&
					bloompg_integral_value((Datum) 0, key->build_type, &ignored);
			}
			pending_transfers = lappend(pending_transfers, pending);
		}
		if (pending_transfers == NIL)
		{
			appendStringInfo(&detail, " idle=%u", source->rti);
			continue;
		}
		INSTR_TIME_SET_CURRENT(source_started);
		if (bloompg_log_transfer_steps)
			elog(LOG, "bloompg transfer source %u start estimated=%.0f base=%.0f incoming=%d outgoing=%d materialized=%s",
				source->rti, source->current_rows, source->base_rows,
				list_length(source->filters), list_length(pending_transfers),
				source->materialized ? "yes" : "no");
		if (!source->materialized && !source->prefetched)
			(void) bloompg_prefetch_active_pair(source, states, relation_count,
				graph, candidate, query_string, bound_params, runtime_owner,
				query_parallel_safe, &next_plan_node_id,
				&next_parallel_wave_id);
		if (source->prefetched)
		{
			source->materialized = true;
			source->materialization_token = source->prefetch_token;
			source->prefetched = false;
			source->prefetch_token = 0;
			source->prefetch_rows = 0;
		}

		if (source->materialized)
		{
			BloomPGRuntimeMaterializationEntry *entry;

			materialization_token = source->materialization_token;
			rows_seen = bloompg_compact_materialization(source,
				pending_transfers, &descriptor);
			entry = bloompg_runtime_find_materialization(materialization_token,
				GetActiveSnapshot());
			if (entry == NULL)
				elog(ERROR, "bloompg lost compacted relation %u", source->rti);
			store = entry->store;
		}
		else
		{
			materialization_token = bloompg_runtime_next_token();
			store = bloompg_begin_runtime_store(runtime_owner);
			/* Register immediately so the planner catch path can close a spilled
			 * store even when execution errors before its descriptor is known. */
			bloompg_runtime_register_materialization(materialization_token,
				GetActiveSnapshot(), store, NULL, 0);
			rows_seen = bloompg_execute_state_source(source, candidate,
				source->plan, pending_transfers, store, &descriptor,
				query_string, bound_params, query_parallel_safe,
				&next_plan_node_id);
			bloompg_runtime_register_materialization(materialization_token,
				GetActiveSnapshot(), store, descriptor, rows_seen);
			/* Executing the source consumed every previously installed Apply
			 * wrapper; their End callbacks own those filter lifetimes. */
			source->filters = NIL;
		}
		if (bloompg_log_transfer_steps)
			elog(LOG, "bloompg transfer source %u finish rows=" UINT64_FORMAT
				" elapsed=%.3fms store=%s bytes=%zu",
				source->rti, rows_seen, bloompg_transfer_elapsed_ms(source_started),
				bloompg_column_store_spilled(store) ? "spill" : "column",
				bloompg_column_store_memory_bytes(store));
		source->current_rows = (double) rows_seen;
		excited = bloompg_is_excited(source->current_rows,
								 source->excitation_rows);
		source->excitation_rows = source->current_rows;
		outcome.build_rows += rows_seen;

		/* Sampling and optimizer estimates only nominate a source.  A formal
		 * scan must confirm the drop before its filter can excite another
		 * relation.  A source that already received a filter is still retained
		 * as its exact reduced materialization even when the new drop is below
		 * the configured threshold. */
		if (!excited && !source->changed)
		{
			bloompg_runtime_release_materialization(materialization_token);
			bloompg_discard_pending_filters(pending_transfers);
			appendStringInfo(&detail,
							 " noexcite=%u:" UINT64_FORMAT "/%.0f",
							 source->rti, rows_seen, source->excitation_rows);
			continue;
		}
		memory_plan = bloompg_make_memory_scan(copyObject(source->fallback_plan),
											 materialization_token, rows_seen,
											 next_plan_node_id++);
		candidate->planTree = bloompg_replace_plan_pointer(candidate->planTree,
													 source->plan, memory_plan,
													 &source_replaced);
		if (!source_replaced)
			elog(ERROR, "bloompg lost source relation %u while materializing it",
				 source->rti);
		source->plan = memory_plan;
		source->materialized = true;
		source->materialization_token = materialization_token;
		source->changed = true;

		if (!excited)
		{
			bloompg_discard_pending_filters(pending_transfers);
			appendStringInfo(&detail,
							 " noexcite=%u:" UINT64_FORMAT "/%.0f",
							 source->rti, rows_seen, source->excitation_rows);
			continue;
		}
		{
			instr_time filter_finalize_started;
			double filter_finalize_ms;
			uint64 one_pass_bitmap_builds;
			uint64 filter_finalize_rows;

			INSTR_TIME_SET_CURRENT(filter_finalize_started);
			filter_finalize_rows = bloompg_finalize_pending_filters(
				pending_transfers, store, descriptor, rows_seen,
				&one_pass_bitmap_builds);
			filter_finalize_ms =
				bloompg_transfer_elapsed_ms(filter_finalize_started);
			source->filter_finalize_ms += filter_finalize_ms;
			source->filter_build_ms += filter_finalize_ms;
			source->exact_scan_ms += filter_finalize_ms;
			source->one_pass_bitmap_builds += one_pass_bitmap_builds;
			if (filter_finalize_rows > 0)
				source->filter_finalize_rescans++;
			source->filter_finalize_rows += filter_finalize_rows;
		}
		foreach(cell, pending_transfers)
		{
			BloomPGPendingTransfer *pending = lfirst(cell);

			bloompg_remember_built_key_statistics(source, pending, rows_seen);
		}
		bloompg_equality_domains_prepare_source(equality_domains, source,
			pending_transfers);

		foreach(cell, pending_transfers)
		{
			BloomPGPendingTransfer *pending = lfirst(cell);
			BloomPGRelationState *destination = pending->destination;
			Plan *wrapper;
			bool replaced = false;
			bool carries_new_lineage;
			BloomPGSampleFilter *sample_filter;
			bool resampled;
			bool sample_cache_hit;
			bool intermediate_sample;
			double prior_rows;
			double estimated_rows;

			if (bloompg_equality_domain_suppresses(equality_domains, source,
				pending))
			{
				appendStringInfo(&detail, " domain_skip=%u->%u:v" UINT64_FORMAT,
					source->rti, destination->rti,
					pending->equality_domain_version);
				if (pending->bitmap_candidate_words != NULL)
				{
					pfree(pending->bitmap_candidate_words);
					pending->bitmap_candidate_words = NULL;
				}
				bloompg_filter_free(pending->filter);
				pending->filter = NULL;
				continue;
			}
			bloompg_filter_set_valid(pending->filter);
			bloompg_runtime_register_filter(pending->spec.token,
											 GetActiveSnapshot(), pending->filter);
			wrapper = bloompg_make_apply_scan(destination->plan,
											copyObject(source->plan),
											&pending->spec, next_plan_node_id++);
			candidate->planTree = bloompg_replace_plan_pointer(candidate->planTree,
														 destination->plan,
														 wrapper, &replaced);
			if (!replaced)
				elog(ERROR, "bloompg lost relation %u while installing transfer",
					 destination->rti);
			destination->plan = wrapper;
			destination->changed = true;
			sample_filter = palloc0(sizeof(BloomPGSampleFilter));
			sample_filter->token = pending->spec.token;
			sample_filter->filter = pending->filter;
			sample_filter->nkeys = pending->spec.nkeys;
			sample_filter->attributes = palloc_array(AttrNumber,
				pending->spec.nkeys);
			sample_filter->materialized_attributes = palloc_array(AttrNumber,
				pending->spec.nkeys);
			memcpy(sample_filter->attributes,
				pending->spec.probe_base_attributes,
				sizeof(AttrNumber) * pending->spec.nkeys);
			memcpy(sample_filter->materialized_attributes,
				pending->spec.probe_attributes,
				sizeof(AttrNumber) * pending->spec.nkeys);
			sample_filter->lineage = bms_copy(source->lineage);
			destination->filters = lappend(destination->filters, sample_filter);
			*pending->sent_lineage = bms_union(*pending->sent_lineage,
														 source->lineage);
			carries_new_lineage = !bms_is_subset(source->lineage,
													 destination->lineage);
			destination->lineage = bms_union(destination->lineage,
											  source->lineage);
			bloompg_equality_domain_mark_applied(equality_domains, pending,
				destination);
			if (carries_new_lineage)
			{
				BloomPGRuntimeMaterializationEntry *destination_entry = NULL;

				prior_rows = destination->current_rows;
				intermediate_sample = false;
				if (destination->materialized)
					destination_entry = bloompg_runtime_find_materialization(
						destination->materialization_token, GetActiveSnapshot());
				else if (destination->prefetched)
					destination_entry = bloompg_runtime_find_materialization(
						destination->prefetch_token, GetActiveSnapshot());
				if (destination_entry != NULL && destination_entry->store != NULL &&
					destination_entry->descriptor != NULL)
				{
					estimated_rows = bloompg_sample_materialized_relation(
						destination_entry->store, destination_entry->descriptor,
						destination->filters,
						&destination->intermediate_sampling_profile);
					destination->filter_sample_fraction =
						destination->intermediate_sampling_profile.last_fraction;
					resampled = true;
					sample_cache_hit = false;
					intermediate_sample = true;
				}
				else
					estimated_rows = bloompg_sample_filtered_relation(candidate,
						destination->rti, destination->fallback_plan,
						destination->base_rows, destination->filters, query_string,
						bound_params, &destination->sample_workspace,
						&destination->sampling_profile, &resampled,
						&sample_cache_hit,
						&destination->filter_sample_fraction);
				if (!resampled)
				{
					double source_fraction = source->base_rows > 0.0 ?
						source->current_rows / source->base_rows : 0.0;

					estimated_rows = prior_rows *
						Min(Max(source_fraction, 0.0), 1.0);
					destination->filter_sample_fraction =
						Min(Max(source_fraction, 0.0), 1.0);
				}
				destination->current_rows = Max(estimated_rows, 0.0);
				destination->active = bloompg_is_excited(
					destination->current_rows, destination->excitation_rows);
				if (destination->active &&
					destination->native_parameterized_index &&
					(contain_subplans((Node *) destination->fallback_plan->qual) ||
					 !bloompg_index_prefilter_available(destination)))
				{
					destination->active = false;
					destination->native_index_guarded = true;
					appendStringInfo(&detail,
						" native_index_guard=%u", destination->rti);
				}
				appendStringInfo(&detail, " estimate=%u:%.0f/%.0f%s%s",
					destination->rti, destination->current_rows,
					destination->excitation_rows,
					intermediate_sample ? "(intermediate)" :
						(resampled ? "(sample)" : "(derived)"),
					sample_cache_hit ? "(cached)" : "");
			}

			outcome.applied = true;
			outcome.steps++;
			outcome.filters++;
			if (collect_profile)
				bloompg_transfer_profile_append_step(&profile_steps, outcome.steps,
					source, pending, rows_seen, materialization_token);
			appendStringInfo(&detail,
							 " step%d=%u->%u keys=%d rows=" UINT64_FORMAT
							 " token=" UINT64_FORMAT " memory=" UINT64_FORMAT
							 " kind=%s",
							 outcome.steps, source->rti, destination->rti,
							 pending->spec.nkeys, rows_seen, pending->spec.token,
							 materialization_token,
							 pending->filter->kind == BLOOMPG_FILTER_BITMAP ?
							 "bitmap" : "bloom");
			appendStringInfo(&detail, " cache=%s",
				pending->build_filter ? "miss" : "hit");
		}
	}
	bloompg_equality_domains_finish(equality_domains, &outcome, &detail);

	if (!outcome.native_fallback)
		bloompg_finalize_replacements(candidate, graph, states, relation_count,
			query_string, bound_params, runtime_owner, query_parallel_safe,
			&next_plan_node_id, &outcome, &detail);
	outcome.profile_json = bloompg_transfer_finish_profile(states,
		relation_count, outcome, &detail,
		collect_profile ? &profile_steps : NULL);
	outcome.detail = detail.data;
	return outcome;
}
