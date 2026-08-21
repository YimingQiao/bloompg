#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/graph.h"
#include "bloompg/path_transform.h"
#include "bloompg/planner.h"
#include "bloompg/profile.h"
#include "bloompg/replan.h"
#include "bloompg/runtime.h"
#include "bloompg/scan.h"
#include "bloompg/transfer.h"

#include "lib/stringinfo.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "portability/instr_time.h"
#include "utils/memutils.h"
#include "utils/errcodes.h"
#include "utils/resowner.h"

static planner_hook_type previous_planner_hook = NULL;
static bool bloompg_inside_planner = false;

static double
bloompg_planner_elapsed_ms(instr_time started)
{
	instr_time elapsed;

	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	return INSTR_TIME_GET_MILLISEC(elapsed);
}

static bool
bloompg_profile_token_exists(List *tokens, uint64 token)
{
	ListCell *cell;

	foreach(cell, tokens)
	{
		uint64 *existing = lfirst(cell);

		if (*existing == token)
			return true;
	}
	return false;
}

static List *
bloompg_profile_materialization_tokens(List *replacements)
{
	List	   *tokens = NIL;
	ListCell *cell;

	foreach(cell, replacements)
	{
		BloomPGReplacement *replacement = lfirst(cell);
		uint64 candidates[2];
		int candidate_count = 0;
		int index;

		if (replacement->kind == BLOOMPG_REPLACEMENT_FILTER)
			continue;
		if (replacement->token != 0)
			candidates[candidate_count++] = replacement->token;
		if (replacement->kind == BLOOMPG_REPLACEMENT_TID &&
			replacement->fallback_token != 0)
			candidates[candidate_count++] = replacement->fallback_token;
		for (index = 0; index < candidate_count; index++)
		{
			uint64 *token;

			if (bloompg_profile_token_exists(tokens, candidates[index]))
				continue;
			token = palloc(sizeof(uint64));
			*token = candidates[index];
			tokens = lappend(tokens, token);
		}
	}
	return tokens;
}

static bool
bloompg_p1_has_parallel_work(List *replacements)
{
	ListCell *cell;

	foreach(cell, replacements)
	{
		BloomPGReplacement *replacement = lfirst(cell);

		if (replacement->parallel_dsm_allowed &&
			replacement->kind != BLOOMPG_REPLACEMENT_EMPTY &&
			replacement->rows >= (double) bloompg_p1_parallel_min_rows)
			return true;
	}
	return false;
}

static bool bloompg_query_scope_is_safe(Query *query);
static PlannedStmt *bloompg_call_next_planner(Query *parse,
											  const char *query_string,
											  int cursor_options,
											  ParamListInfo bound_params);

static bool
bloompg_external_param_walker(Node *node, void *context)
{
	(void) context;
	if (node == NULL)
		return false;
	if (IsA(node, Param))
		return castNode(Param, node)->paramkind == PARAM_EXTERN;
	if (IsA(node, Query))
		return query_tree_walker(castNode(Query, node),
								 bloompg_external_param_walker, NULL, 0);
	return expression_tree_walker(node, bloompg_external_param_walker, NULL);
}

static bool
bloompg_unsafe_sublink_walker(Node *node, void *context)
{
	(void) context;
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink *link = castNode(SubLink, node);

		if (!IsA(link->subselect, Query) ||
			!bloompg_query_scope_is_safe(castNode(Query, link->subselect)))
			return true;
		return bloompg_unsafe_sublink_walker((Node *) link->testexpr, NULL);
	}
	if (IsA(node, Query))
		return !bloompg_query_scope_is_safe(castNode(Query, node));
	return expression_tree_walker(node, bloompg_unsafe_sublink_walker, NULL);
}

static bool
bloompg_query_scope_is_safe(Query *query)
{
	ListCell *cell;

	if (query->commandType != CMD_SELECT || query->hasModifyingCTE ||
		query->hasRecursive || query->rowMarks != NIL ||
		query->hasRowSecurity ||
		contain_volatile_functions((Node *) query))
		return false;
	foreach(cell, query->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, cell);

		/* A lateral relation is parameterized by construction and therefore
		 * cannot be executed as the independent Bloom table operator that the
		 * transfer phase requires. */
		if (rte->tablesample != NULL || rte->lateral)
			return false;
		if (rte->rtekind == RTE_RELATION &&
			rte->relkind != RELKIND_RELATION &&
			rte->relkind != RELKIND_PARTITIONED_TABLE &&
			rte->relkind != RELKIND_MATVIEW &&
			rte->relkind != RELKIND_VIEW)
			return false;
		if (rte->rtekind == RTE_SUBQUERY &&
			!bloompg_query_scope_is_safe(rte->subquery))
			return false;
	}
	foreach(cell, query->cteList)
	{
		CommonTableExpr *cte = lfirst_node(CommonTableExpr, cell);

		if (cte->cterecursive || !IsA(cte->ctequery, Query) ||
			!bloompg_query_scope_is_safe(castNode(Query, cte->ctequery)))
			return false;
	}
	if (query->hasSubLinks &&
		query_tree_walker(query, bloompg_unsafe_sublink_walker, NULL,
			QTW_IGNORE_RT_SUBQUERIES | QTW_IGNORE_CTE_SUBQUERIES))
		return false;
	return true;
}

static bool
bloompg_query_is_safe(Query *query, ParamListInfo bound_params)
{
	return bloompg_query_scope_is_safe(query) &&
		!(bound_params == NULL &&
		  bloompg_external_param_walker((Node *) query, NULL));
}

/* P0 is an executable analysis plan, not the plan returned to the user.  A
 * parameterized nested-loop inner path cannot be detached and scanned on its
 * own.  Temporarily discouraging nested loops lets PostgreSQL expose
 * unparameterized hash/merge inputs for equality-join Bloom.  P1 is planned
 * after this flag is restored and retains the user's normal join choices. */
static PlannedStmt *
bloompg_make_analysis_plan(Query *parse,
						   const char *query_string,
						   int cursor_options,
						   ParamListInfo bound_params)
{
	PlannedStmt *result = NULL;
	bool saved_enable_nestloop = enable_nestloop;
	bool saved_enable_indexonlyscan = enable_indexonlyscan;

	enable_nestloop = false;
	if (bloompg_late_materialize)
		enable_indexonlyscan = false;
	PG_TRY();
	{
		result = bloompg_call_next_planner(parse, query_string,
										 cursor_options, bound_params);
	}
	PG_FINALLY();
	{
		enable_nestloop = saved_enable_nestloop;
		enable_indexonlyscan = saved_enable_indexonlyscan;
	}
	PG_END_TRY();
	return result;
}

static PlannedStmt *
bloompg_call_next_planner(Query *parse,
						  const char *query_string,
						  int cursor_options,
						  ParamListInfo bound_params)
{
	if (previous_planner_hook != NULL)
		return previous_planner_hook(parse, query_string, cursor_options, bound_params);
	return standard_planner(parse, query_string, cursor_options, bound_params);
}

PlannedStmt *
bloompg_plan_auxiliary_query(Query *query, const char *query_string,
	int cursor_options, ParamListInfo bound_params)
{
	Assert(query != NULL);
	Assert(bloompg_inside_planner);
	return bloompg_call_next_planner(query, query_string, cursor_options,
		bound_params);
}

PlannedStmt *
bloompg_planner(Query *parse,
				const char *query_string,
				int cursor_options,
				ParamListInfo bound_params)
{
	PlannedStmt *result;

	if (!bloompg_enabled || bloompg_inside_planner ||
		parse->commandType != CMD_SELECT ||
		!bloompg_query_is_safe(parse, bound_params))
		return bloompg_call_next_planner(parse, query_string, cursor_options, bound_params);

	bloompg_inside_planner = true;
	PG_TRY();
	{
		BloomPGJoinGraph *graph;
		BloomPGTransferOutcome transfer;
		char *trace;
		Query *analysis_query = copyObject(parse);
		int bloom_cursor_options = cursor_options & ~CURSOR_OPT_PARALLEL_OK;
		bool query_parallel_safe =
			max_parallel_hazard(analysis_query) == PROPARALLEL_SAFE;
		instr_time phase_started;
		double p0_ms;
		double graph_ms;
		instr_time planner_started;

		INSTR_TIME_SET_CURRENT(planner_started);

		/*
		 * Build P0 before Bloom executes any detached relation subplans.  Nested
		 * planners are protected by bloompg_inside_planner.
		 */
		INSTR_TIME_SET_CURRENT(phase_started);
		result = bloompg_make_analysis_plan(copyObject(analysis_query), query_string,
										bloom_cursor_options, bound_params);
		p0_ms = bloompg_planner_elapsed_ms(phase_started);
		INSTR_TIME_SET_CURRENT(phase_started);
		graph = bloompg_graph_build(analysis_query, CurrentMemoryContext);
		bloompg_graph_augment_plan(graph, result);
		trace = bloompg_graph_trace(graph);
		graph_ms = bloompg_planner_elapsed_ms(phase_started);
		if (graph->edges != NIL)
		{
			StringInfoData detail;
			BloomPGPlanProfileInput profile_input;
			volatile bool replanned = false;
			volatile bool transfer_failed = false;
			volatile bool p1_parallel_biased = false;
			volatile int transfer_error_code = 0;
			volatile double transfer_ms = 0.0;
			volatile double p1_ms = 0.0;
			double native_preview_ms;
			PlannedStmt *native_preview;
			Bitmapset *native_parameterized_indexes;
			uint64 p1_cardinality_adjustments = 0;
			double p1_cardinality_max_ratio = 1.0;
			uint64 runtime_mark = bloompg_runtime_next_token();
			MemoryContext planner_context = CurrentMemoryContext;
			ResourceOwner planner_owner = CurrentResourceOwner;
			instr_time transfer_started;
			instr_time p1_started;
			char *p1_path_transform_json;

			MemSet(&transfer, 0, sizeof(transfer));
			bloompg_path_transform_reset();
			INSTR_TIME_SET_CURRENT(p1_started);
			native_preview = bloompg_call_next_planner(copyObject(analysis_query),
				query_string, cursor_options, bound_params);
			native_preview_ms = bloompg_planner_elapsed_ms(p1_started);
			native_parameterized_indexes =
				bloompg_parameterized_index_relids(native_preview);
			INSTR_TIME_SET_CURRENT(transfer_started);
			BeginInternalSubTransaction("bloompg optimizer execution");
			PG_TRY(_transfer);
			{
				transfer = bloompg_apply_transfers(result, analysis_query, graph,
											 query_string, cursor_options, bound_params,
											 query_parallel_safe,
											 native_parameterized_indexes);
				ReleaseCurrentSubTransaction();
				MemoryContextSwitchTo(planner_context);
				CurrentResourceOwner = planner_owner;
				if (transfer.native_fallback)
					bloompg_runtime_cleanup_since(runtime_mark);
				transfer_ms = bloompg_planner_elapsed_ms(transfer_started);
			}
			PG_CATCH(_transfer);
			{
				ErrorData *error_data;
				bool must_rethrow;

				MemoryContextSwitchTo(planner_context);
				error_data = CopyErrorData();
				FlushErrorState();
				bloompg_runtime_cleanup_since(runtime_mark);
				RollbackAndReleaseCurrentSubTransaction();
				MemoryContextSwitchTo(planner_context);
				CurrentResourceOwner = planner_owner;
				must_rethrow = error_data->sqlerrcode == ERRCODE_QUERY_CANCELED ||
					error_data->sqlerrcode == ERRCODE_ADMIN_SHUTDOWN ||
					error_data->sqlerrcode == ERRCODE_CRASH_SHUTDOWN ||
					error_data->sqlerrcode == ERRCODE_OUT_OF_MEMORY ||
					ERRCODE_TO_CATEGORY(error_data->sqlerrcode) ==
						ERRCODE_INTERNAL_ERROR;
				if (must_rethrow)
					ReThrowError(error_data);
				transfer_failed = true;
				transfer_error_code = error_data->sqlerrcode;
				transfer_ms = bloompg_planner_elapsed_ms(transfer_started);
				FreeErrorData(error_data);
			}
			PG_END_TRY(_transfer);
			if (transfer.applied && bloompg_replan_after_transfer &&
				transfer.replacements != NIL)
			{
				PlannedStmt *volatile replacement_plan = NULL;
				double saved_parallel_setup_cost = parallel_setup_cost;
				double saved_parallel_tuple_cost = parallel_tuple_cost;
				int saved_min_parallel_table_scan_size =
					min_parallel_table_scan_size;
				int saved_min_parallel_index_scan_size =
					min_parallel_index_scan_size;

				bloompg_replan_begin(transfer.replacements, graph);
				INSTR_TIME_SET_CURRENT(p1_started);
				if (bloompg_p1_parallel_bias && query_parallel_safe &&
					max_parallel_workers_per_gather > 0 &&
					bloompg_p1_has_parallel_work(transfer.replacements))
				{
					/* Exact, already-resident P1 inputs have neither disk-scan risk nor
					 * the cardinality uncertainty represented by core's default Gather
					 * penalties.  Bias only this second optimizer invocation; P0 and
					 * ordinary PostgreSQL queries retain the user's settings. */
					parallel_setup_cost = 0.0;
					parallel_tuple_cost = 0.0;
					min_parallel_table_scan_size = 0;
					min_parallel_index_scan_size = 0;
					p1_parallel_biased = true;
				}
				PG_TRY(_replan);
				{
					replacement_plan = bloompg_call_next_planner(
						copyObject(analysis_query), query_string,
						cursor_options, bound_params);
				}
				PG_FINALLY(_replan);
				{
					parallel_setup_cost = saved_parallel_setup_cost;
					parallel_tuple_cost = saved_parallel_tuple_cost;
					min_parallel_table_scan_size =
						saved_min_parallel_table_scan_size;
					min_parallel_index_scan_size =
						saved_min_parallel_index_scan_size;
					bloompg_replan_end();
				}
				PG_END_TRY(_replan);
				bloompg_prepare_parallel_plan(
					(PlannedStmt *) replacement_plan);
				bloompg_replan_cardinality_stats(
					&p1_cardinality_adjustments,
					&p1_cardinality_max_ratio);
				p1_ms = bloompg_planner_elapsed_ms(p1_started);
				if (replacement_plan != NULL)
				{
					result = replacement_plan;
					replanned = true;
				}
			}
			/* P0 is always an analysis artifact: it was planned with nested loops
			 * discouraged and may also have index-only scans disabled.  Returning it
			 * when transfer nominated no replacement can be orders of magnitude
			 * slower than the user's native plan (TPC-H Q15 is a concrete case).
			 * Replan with the original settings even when projection preparation did
			 * not mutate the P0 tree. */
			if (!transfer.applied || transfer_failed)
			{
				result = native_preview;
				replanned = true;
			}
			bloompg_finish_replacements(transfer.replacements, result);
			p1_path_transform_json = bloompg_path_transform_json();
			initStringInfo(&detail);
			appendStringInfoString(&detail, trace);
			appendStringInfo(&detail,
				" phases=p0:%.3fms native:%.3fms graph:%.3fms"
					" transfer:%.3fms p1:%.3fms"
					" p1_parallel=%s"
					" p1_cardinality=" UINT64_FORMAT ":%.2fx",
					p0_ms, native_preview_ms, graph_ms, (double) transfer_ms,
					(double) p1_ms,
					p1_parallel_biased ? "biased" : "unchanged",
					p1_cardinality_adjustments, p1_cardinality_max_ratio);
			appendStringInfo(&detail, " p1_paths=%s",
				p1_path_transform_json);
			if (transfer_failed)
				appendStringInfo(&detail,
					" fallback=optimizer_execution_error sqlstate=%s",
					unpack_sql_state(transfer_error_code));
			else if (transfer.applied)
				appendStringInfo(&detail,
								 " transfers=%d build_rows=" UINT64_FORMAT
								 " replan=%s%s",
								 transfer.steps, transfer.build_rows,
								 replanned ? "yes" : "no",
									 transfer.detail != NULL ? transfer.detail : "");
			else if (transfer.detail != NULL && transfer.detail[0] != '\0')
				appendStringInfoString(&detail, transfer.detail);
			bloompg_set_last_trace(detail.data);
			MemSet(&profile_input, 0, sizeof(profile_input));
			profile_input.transfer_trace = detail.data;
			profile_input.transfer_json = transfer.profile_json;
			profile_input.p1_path_transform_json =
				p1_path_transform_json;
			profile_input.planning_ms = bloompg_planner_elapsed_ms(planner_started);
			profile_input.p0_ms = p0_ms;
			profile_input.native_preview_ms = native_preview_ms;
			profile_input.graph_ms = graph_ms;
			profile_input.transfer_ms = transfer_ms;
			profile_input.statistics_ms = transfer.statistics_ms;
			profile_input.p1_ms = p1_ms;
			profile_input.p1_cardinality_adjustments =
				p1_cardinality_adjustments;
			profile_input.p1_cardinality_max_ratio =
				p1_cardinality_max_ratio;
			profile_input.replanned = replanned;
			profile_input.p1_parallel_biased = p1_parallel_biased;
			profile_input.transfer_failed = transfer_failed;
			profile_input.transfer_error_code = transfer_error_code;
			profile_input.materialization_tokens =
				bloompg_profile_materialization_tokens(transfer.replacements);
			bloompg_profile_remember_plan(result, query_string, &profile_input);
			pfree(trace);
			pfree(detail.data);
			pfree(p1_path_transform_json);
		}
		else if (graph->tables != NIL)
		{
			/*
			 * P0 deliberately disables parallel paths because transfer executes
			 * detached subplans inside the planning backend.  When the join graph
			 * has no transferable edge, P0 is only an analysis artifact: returning
			 * it would silently disable PostgreSQL parallelism for otherwise
			 * untouched queries (TPC-H Q1 is the canonical example).  Replan with
			 * the caller's original cursor options for the actual executor plan.
			 */
			result = bloompg_call_next_planner(copyObject(analysis_query),
				query_string, cursor_options, bound_params);
			bloompg_set_last_trace(trace);
			pfree(trace);
		}
	}
	PG_FINALLY();
	{
		bloompg_inside_planner = false;
	}
	PG_END_TRY();

	return result;
}

void
bloompg_planner_init(void)
{
	previous_planner_hook = planner_hook;
	planner_hook = bloompg_planner;
}

void
bloompg_planner_fini(void)
{
	if (planner_hook == bloompg_planner)
		planner_hook = previous_planner_hook;
}
