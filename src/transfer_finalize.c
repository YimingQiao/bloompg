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

#include <float.h>

/* Seal relation outcomes and publish P1 replacement metadata. */

void
bloompg_finalize_replacements(PlannedStmt *candidate,
	BloomPGJoinGraph *graph, BloomPGRelationState **states,
	int relation_count, const char *query_string,
	ParamListInfo bound_params, ResourceOwner runtime_owner,
	bool query_parallel_safe, int *next_plan_node_id_target,
	BloomPGTransferOutcome *outcome_target, StringInfoData *detail_target)
{
	BloomPGTransferOutcome outcome = *outcome_target;
	StringInfoData detail = *detail_target;
	int next_plan_node_id = *next_plan_node_id_target;

	for (int rti = 1; rti <= relation_count; rti++)
	{
		BloomPGRelationState *state = states[rti];
		BloomPGReplacement *replacement;
		BloomPGColumnStore *final_store;
		BloomPGRuntimeMaterializationEntry *full_entry;
		TupleDesc final_descriptor = NULL;
		uint64 final_rows;
		uint64 final_token;
		Plan *final_memory;
		bool replaced = false;
		bool reused;

		if (state == NULL)
			continue;
		if (!state->changed)
		{
			if (state->prefetched)
			{
				bloompg_runtime_release_materialization(state->prefetch_token);
				state->prefetched = false;
				state->prefetch_token = 0;
				state->prefetch_rows = 0;
			}
			continue;
		}
		if (state->prefetched)
		{
			state->materialized = true;
			state->materialization_token = state->prefetch_token;
			state->prefetched = false;
			state->prefetch_token = 0;
			state->prefetch_rows = 0;
		}
		/* If sampling says the newly attached formal filters did not excite the
		 * destination, keep its native P1 access path and apply those filters at
		 * execution.  This is the PostgreSQL FILTERED form; unlike a sample-zero,
		 * it never claims exact emptiness. */
		if (!state->materialized && bloompg_replan_after_transfer &&
			state->table->rte_kind == RTE_RELATION && state->filters != NIL)
		{
			replacement = palloc0(sizeof(BloomPGReplacement));
			replacement->kind = BLOOMPG_REPLACEMENT_FILTER;
			replacement->rti = state->replacement_rti;
			replacement->rows = state->current_rows;
			replacement->filters = state->filters;
			outcome.replacements = lappend(outcome.replacements, replacement);
			state->replacement_kind = "filter";
			appendStringInfo(&detail, " filtered=%u:%d", state->rti,
				list_length(state->filters));
			continue;
		}
		reused = bloompg_memory_scan_info(state->plan, &final_token,
			&final_rows) &&
			bloompg_runtime_find_materialization(final_token,
				GetActiveSnapshot()) != NULL;
		if (reused)
		{
			final_memory = state->plan;
			appendStringInfo(&detail, " reuse=%u:" UINT64_FORMAT,
				state->rti, final_token);
		}
		else
		{
			if (state->materialized)
			{
				final_token = state->materialization_token;
				final_rows = bloompg_compact_materialization(state, NIL,
					&final_descriptor);
			}
			else
			{
				final_token = bloompg_runtime_next_token();
				final_store = bloompg_begin_runtime_store(runtime_owner);
				bloompg_runtime_register_materialization(final_token,
					GetActiveSnapshot(), final_store, NULL, 0);
				final_rows = bloompg_execute_state_source(state, candidate,
					state->plan, NIL, final_store, &final_descriptor,
					query_string, bound_params, query_parallel_safe,
					&next_plan_node_id);
				state->filters = NIL;
				bloompg_runtime_register_materialization(final_token,
					GetActiveSnapshot(), final_store, final_descriptor,
					final_rows);
			}
			final_memory = bloompg_make_memory_scan(copyObject(state->fallback_plan),
											 final_token, final_rows,
											 next_plan_node_id++);
			candidate->planTree = bloompg_replace_plan_pointer(candidate->planTree,
												 state->plan, final_memory,
												 &replaced);
			if (!replaced)
				elog(ERROR, "bloompg lost relation %u while finalizing it",
					 state->rti);
		}
		state->plan = final_memory;
		state->materialized = true;
		state->materialization_token = final_token;
		state->current_rows = (double) final_rows;
		replacement = palloc0(sizeof(BloomPGReplacement));
		replacement->kind = BLOOMPG_REPLACEMENT_MEMORY;
		state->replacement_kind = "memory";
		replacement->rti = state->replacement_rti;
		replacement->token = final_token;
		replacement->rows = state->current_rows;
		replacement->native_parameterized_paths_equivalent =
			!state->materialization_used_filters;
		if (final_rows == 0)
		{
			replacement->kind = BLOOMPG_REPLACEMENT_EMPTY;
			state->replacement_kind = "empty";
			appendStringInfo(&detail, " empty=%u:" UINT64_FORMAT,
				state->rti, final_token);
		}
		else if (state->tid_eligible && final_rows < (uint64) state->base_rows)
		{
			uint64 tid_token;

			full_entry = bloompg_runtime_find_materialization(final_token,
				GetActiveSnapshot());
			tid_token = bloompg_create_tid_materialization(full_entry,
				state->ctid_attribute, final_rows, runtime_owner);

			if (tid_token != 0)
			{
				replacement->kind = BLOOMPG_REPLACEMENT_TID;
				state->replacement_kind = "tid";
				replacement->fallback_token = final_token;
				replacement->token = tid_token;
				appendStringInfo(&detail, " tid=%u:" UINT64_FORMAT,
					state->rti, tid_token);
			}
		}
		if (state->table->rte_kind == RTE_SUBQUERY &&
			!state->simple_subquery)
			replacement->targetlist =
				bloompg_subquery_replacement_targetlist(state);
		else
			replacement->targetlist = (List *) bloompg_normalize_syntax_vars(
				(Node *) bloompg_semantic_targetlist(state->plan), NULL);
		if (replacement->targetlist != NIL)
		{
			full_entry = bloompg_runtime_find_materialization(final_token,
				GetActiveSnapshot());
			if (full_entry == NULL || full_entry->store == NULL ||
				full_entry->descriptor == NULL)
				elog(ERROR, "bloompg lost relation %u while finalizing statistics",
					state->rti);
			state->statistics = bloompg_finalize_state_statistics(state, states,
				relation_count, graph, full_entry);
			replacement->statistics = state->statistics;
			outcome.statistics_ms += state->statistics->finalize_ms;
			outcome.statistics_rows += state->statistics->rows_examined;
			outcome.statistics_values += state->statistics->values_examined;
			outcome.statistics_key_sets +=
				list_length(state->statistics->join_keys);
			appendStringInfo(&detail,
				" stats=%u:" UINT64_FORMAT ":%d:%.3fms",
				state->rti, state->statistics->rows_examined,
				list_length(state->statistics->join_keys),
				state->statistics->finalize_ms);
			outcome.replacements = lappend(outcome.replacements, replacement);
		}
		else
			appendStringInfo(&detail, " skip_replacement=%u", state->rti);
	}
	*outcome_target = outcome;
	*detail_target = detail;
	*next_plan_node_id_target = next_plan_node_id;
}
