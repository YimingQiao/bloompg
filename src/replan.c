#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/scan.h"
#include "bloompg/path_transform.h"
#include "bloompg/replan.h"

#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "catalog/pg_class.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"

static set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = NULL;
static set_join_pathlist_hook_type previous_set_join_pathlist_hook = NULL;
static List *active_replacements = NIL;
static const BloomPGJoinGraph *active_graph = NULL;
static uint64 cardinality_adjustments = 0;
static double cardinality_max_ratio = 1.0;

static void bloompg_set_join_pathlist(PlannerInfo *root,
	RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel,
	JoinType jointype, JoinPathExtraData *extra);

static Plan *bloompg_plan_memory_path(PlannerInfo *root,
									 RelOptInfo *rel,
									 CustomPath *best_path,
									 List *targetlist,
									 List *clauses,
									 List *custom_plans);
static Plan *bloompg_plan_tid_path(PlannerInfo *root,
								  RelOptInfo *rel,
								  CustomPath *best_path,
								  List *targetlist,
								  List *clauses,
								  List *custom_plans);
static Plan *bloompg_plan_empty_path(PlannerInfo *root,
								RelOptInfo *rel,
								CustomPath *best_path,
								List *targetlist,
								List *clauses,
								List *custom_plans);

static const CustomPathMethods bloompg_memory_path_methods = {
	.CustomName = "BloomPG Memory",
	.PlanCustomPath = bloompg_plan_memory_path
};

static const CustomPathMethods bloompg_empty_path_methods = {
	.CustomName = "BloomPG Empty",
	.PlanCustomPath = bloompg_plan_empty_path
};

static const CustomPathMethods bloompg_tid_path_methods = {
	.CustomName = "BloomPG TID",
	.PlanCustomPath = bloompg_plan_tid_path
};

static BloomPGReplacement *
bloompg_find_replacement(Index rti)
{
	ListCell *cell;

	foreach(cell, active_replacements)
	{
		BloomPGReplacement *replacement = lfirst(cell);

		if (replacement->rti == rti)
			return replacement;
	}
	return NULL;
}

static bool
bloompg_replacement_is_exact(Index rti)
{
	BloomPGReplacement *replacement = bloompg_find_replacement(rti);

	return replacement != NULL && replacement->rows > 0.0 &&
		(replacement->kind == BLOOMPG_REPLACEMENT_MEMORY ||
		 replacement->kind == BLOOMPG_REPLACEMENT_TID);
}

static BloomPGJoinKeyStatistics *
bloompg_find_join_key_statistics(BloomPGReplacement *replacement,
	Index peer_rti)
{
	ListCell *cell;

	if (replacement == NULL || replacement->statistics == NULL)
		return NULL;
	foreach(cell, replacement->statistics->join_keys)
	{
		BloomPGJoinKeyStatistics *keys = lfirst(cell);

		if (keys->peer_rti == peer_rti)
			return keys;
	}
	return NULL;
}

static bool
bloompg_join_key_statistics_compatible(
	const BloomPGJoinKeyStatistics *outer,
	const BloomPGJoinKeyStatistics *inner)
{
	int key;

	if (outer == NULL || inner == NULL || outer->nkeys != inner->nkeys ||
		outer->nkeys <= 0)
		return false;
	for (key = 0; key < outer->nkeys; key++)
	{
		if (outer->operators[key] != inner->operators[key] ||
			outer->collations[key] != inner->collations[key])
			return false;
	}
	return true;
}

/*
 * Estimate an inner join from statistics sealed after transfer.  Fixed-point
 * transfer makes the smaller surviving key domain approximately contained in
 * the larger one (exactly so for bitmap filters, modulo Bloom false positives
 * otherwise), making the classic max(NDV) denominator appropriate again.
 *
 * If several relation pairs cross the same join boundary, use the strongest
 * composite-key selectivity rather than multiplying them.  This deliberately
 * treats cycle-closing predicates as correlated and avoids recreating the
 * catastrophic multi-clause underestimates that post-transfer statistics are
 * intended to remove.
 */
static bool
bloompg_statistics_join_rows(RelOptInfo *outerrel, RelOptInfo *innerrel,
	double *estimated_rows, int *key_pairs)
{
	double strongest_selectivity = 1.0;
	double cartesian_rows;
	bool found = false;
	ListCell *cell;

	*key_pairs = 0;
	if (outerrel->rows <= 0.0 || innerrel->rows <= 0.0)
		return false;
	foreach(cell, active_replacements)
	{
		BloomPGReplacement *outer_replacement = lfirst(cell);
		ListCell *key_cell;

		if (!bms_is_member(outer_replacement->rti, outerrel->relids) ||
			!bloompg_replacement_is_exact(outer_replacement->rti) ||
			outer_replacement->statistics == NULL ||
			outer_replacement->statistics->rows == 0)
			continue;
		foreach(key_cell, outer_replacement->statistics->join_keys)
		{
			BloomPGJoinKeyStatistics *outer_keys = lfirst(key_cell);
			BloomPGReplacement *inner_replacement;
			BloomPGJoinKeyStatistics *inner_keys;
			double outer_nonnull_fraction;
			double inner_nonnull_fraction;
			double selectivity;

			if (!bms_is_member(outer_keys->peer_rti, innerrel->relids) ||
				!bloompg_replacement_is_exact(outer_keys->peer_rti))
				continue;
			inner_replacement = bloompg_find_replacement(outer_keys->peer_rti);
			inner_keys = bloompg_find_join_key_statistics(inner_replacement,
				outer_replacement->rti);
			if (!bloompg_join_key_statistics_compatible(outer_keys, inner_keys) ||
				inner_replacement->statistics == NULL ||
				inner_replacement->statistics->rows == 0 ||
				outer_keys->ndistinct <= 0.0 || inner_keys->ndistinct <= 0.0)
				continue;
			outer_nonnull_fraction = (double) outer_keys->nonnull_rows /
				(double) outer_replacement->statistics->rows;
			inner_nonnull_fraction = (double) inner_keys->nonnull_rows /
				(double) inner_replacement->statistics->rows;
			selectivity = outer_nonnull_fraction * inner_nonnull_fraction /
				Max(outer_keys->ndistinct, inner_keys->ndistinct);
			selectivity = Min(Max(selectivity, 0.0), 1.0);
			strongest_selectivity = Min(strongest_selectivity, selectivity);
			found = true;
			(*key_pairs)++;
		}
	}
	if (!found)
		return false;
	cartesian_rows = outerrel->rows * innerrel->rows;
	*estimated_rows = clamp_row_est(cartesian_rows * strongest_selectivity);
	return true;
}

static void
bloompg_raise_join_path_rows(Path *path, double rows)
{
	double old_rows;

	if (path == NULL || path->param_info != NULL || path->rows >= rows)
		return;
	old_rows = Max(path->rows, 1.0);
	path->rows = rows;
	/* Preserve the core path comparison while accounting for the additional
	 * tuples projected by the corrected estimate. */
	path->total_cost += (rows - old_rows) * cpu_tuple_cost;
}

static void
bloompg_set_join_pathlist(PlannerInfo *root,
	RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel,
	JoinType jointype, JoinPathExtraData *extra)
{
	double old_rows;
	double statistics_rows;
	double ratio;
	int key_pairs;
	ListCell *cell;

	if (previous_set_join_pathlist_hook != NULL)
		previous_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
			jointype, extra);
	if (active_replacements == NIL || root->parent_root != NULL ||
		jointype != JOIN_INNER ||
		!bloompg_statistics_join_rows(outerrel, innerrel,
			&statistics_rows, &key_pairs))
		return;

	old_rows = Max(joinrel->rows, 1.0);
	/* In the first iteration use transfer statistics as a lower bound.  Core
	 * can still retain a larger estimate from constraints or clauses that the
	 * Bloom graph does not model. */
	if (statistics_rows <= old_rows)
		return;
	ratio = statistics_rows / old_rows;
	joinrel->rows = statistics_rows;
	foreach(cell, joinrel->pathlist)
		bloompg_raise_join_path_rows(lfirst(cell), statistics_rows);
	/* Partial paths expose per-worker rows.  Preserve their prior ratio to the
	 * unpartitioned estimate instead of assigning the full relation count. */
	foreach(cell, joinrel->partial_pathlist)
	{
		Path *path = lfirst(cell);

		if (path->param_info == NULL && path->rows > 0.0)
			bloompg_raise_join_path_rows(path, path->rows * ratio);
	}
	cardinality_adjustments++;
	cardinality_max_ratio = Max(cardinality_max_ratio, ratio);
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg P1 raised inner join rows from %.0f to %.0f (%.2fx) using %d post-transfer key statistic%s",
			old_rows, statistics_rows, ratio, key_pairs,
			key_pairs == 1 ? "" : "s");
}

static bool
bloompg_expressions_compatible(Node *left, Node *right)
{
	left = strip_implicit_coercions(left);
	right = strip_implicit_coercions(right);
	if (IsA(left, Var) && IsA(right, Var))
	{
		Var *left_var = castNode(Var, left);
		Var *right_var = castNode(Var, right);

		return ((left_var->varno == right_var->varno &&
				 left_var->varattno == right_var->varattno) ||
				(left_var->varnosyn == right_var->varnosyn &&
				 left_var->varattnosyn == right_var->varattnosyn) ||
				((Index) left_var->varno == right_var->varnosyn &&
				 left_var->varattno == right_var->varattnosyn) ||
				(left_var->varnosyn == (Index) right_var->varno &&
				 left_var->varattnosyn == right_var->varattno)) &&
			left_var->vartype == right_var->vartype &&
			left_var->vartypmod == right_var->vartypmod &&
			left_var->varcollid == right_var->varcollid;
	}
	return equal(left, right);
}

static bool
bloompg_targetlist_compatible(PathTarget *path_target, List *materialized)
{
	ListCell *path_cell;

	foreach(path_cell, path_target->exprs)
	{
		ListCell *materialized_cell;
		bool found = false;

		foreach(materialized_cell, materialized)
		{
			TargetEntry *entry = lfirst_node(TargetEntry, materialized_cell);

			if (!entry->resjunk &&
				bloompg_expressions_compatible(lfirst(path_cell),
											 (Node *) entry->expr))
			{
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}
	return true;
}

static Path *
bloompg_fallback_path(PlannerInfo *root, RelOptInfo *rel,
					 RangeTblEntry *rte)
{
	ListCell *cell;

	/* Build an independent sequential fallback instead of borrowing an entry
	 * from rel->pathlist.  add_path() is allowed to discard dominated native
	 * paths, and an index-only workload might not retain a SeqPath at all. */
	if (rte->rtekind == RTE_RELATION &&
		rte->relkind != RELKIND_PARTITIONED_TABLE)
		return create_seqscan_path(root, rel, NULL, 0);
	if (rte->rtekind == RTE_CTE)
	{
		foreach(cell, rel->pathlist)
		{
			Path *path = lfirst(cell);
			Path *copy;

			if (path->param_info != NULL || path->pathtype != T_CteScan ||
				!IsA(path, Path))
				continue;
			copy = makeNode(Path);
			memcpy(copy, path, sizeof(Path));
			return copy;
		}
		return NULL;
	}
	if (rte->rtekind == RTE_SUBQUERY)
	{
		foreach(cell, rel->pathlist)
		{
			Path *path = lfirst(cell);
			SubqueryScanPath *copy;

			if (path->param_info != NULL || !IsA(path, SubqueryScanPath))
				continue;
			copy = makeNode(SubqueryScanPath);
			memcpy(copy, path, sizeof(SubqueryScanPath));
			return &copy->path;
		}
		return NULL;
	}

	/* A partitioned parent has no storage of its own.  Its AppendPath is a
	 * top-level container over child paths, so clone that container before the
	 * low-cost memory path can dominate it. */
	foreach(cell, rel->pathlist)
	{
		Path *path = lfirst(cell);

		if (path->param_info == NULL && IsA(path, AppendPath))
		{
			AppendPath *copy = makeNode(AppendPath);

			memcpy(copy, path, sizeof(AppendPath));
			return &copy->path;
		}
	}
	return NULL;
}

static CustomPath *
bloompg_replacement_path(BloomPGReplacement *replacement,
						 Index rti,
						 RelOptInfo *rel,
						 Path *fallback,
						 double rows,
						 bool parallel_aware,
						 int parallel_workers)
{
	CustomPath *path = makeNode(CustomPath);

	Assert(replacement->kind != BLOOMPG_REPLACEMENT_FILTER);
	path->path.pathtype = T_CustomScan;
	path->path.parent = rel;
	path->path.pathtarget = rel->reltarget;
	path->path.param_info = NULL;
	path->path.parallel_aware = parallel_aware;
	path->path.parallel_safe = fallback->parallel_safe;
	path->path.parallel_workers = parallel_workers;
	path->path.rows = rows;
	path->path.disabled_nodes = 0;
	path->path.startup_cost = 0;
	path->path.total_cost = replacement->kind == BLOOMPG_REPLACEMENT_EMPTY ? 0 :
		replacement->kind == BLOOMPG_REPLACEMENT_TID ?
		rows * (cpu_tuple_cost + cpu_operator_cost) : rows * cpu_tuple_cost;
	path->path.pathkeys = NIL;
	path->flags = CUSTOMPATH_SUPPORT_PROJECTION |
		CUSTOMPATH_SUPPORT_BACKWARD_SCAN;
	path->custom_paths = list_make1(fallback);
	path->custom_restrictinfo = NIL;
	if (replacement->kind == BLOOMPG_REPLACEMENT_TID)
	{
		path->custom_private = list_make3(makeInteger((int) rti),
			makeString(psprintf(UINT64_FORMAT, replacement->token)),
			makeString(psprintf(UINT64_FORMAT, (uint64) replacement->rows)));
		path->methods = &bloompg_tid_path_methods;
	}
	else if (replacement->kind == BLOOMPG_REPLACEMENT_MEMORY)
	{
		path->custom_private = list_make4(makeInteger((int) rti),
			makeString(psprintf(UINT64_FORMAT, replacement->token)),
			makeString(psprintf(UINT64_FORMAT, (uint64) replacement->rows)),
			copyObject(replacement->targetlist));
		path->methods = &bloompg_memory_path_methods;
	}
	else if (replacement->kind == BLOOMPG_REPLACEMENT_EMPTY)
	{
		path->custom_private = list_make3(makeInteger((int) rti),
			makeString(psprintf(UINT64_FORMAT, replacement->token)),
			copyObject(replacement->targetlist));
		path->methods = &bloompg_empty_path_methods;
	}
	else
		elog(ERROR, "invalid bloompg relation replacement kind");
	return path;
}

static void
bloompg_set_rel_pathlist(PlannerInfo *root,
						 RelOptInfo *rel,
						 Index rti,
						 RangeTblEntry *rte)
{
	BloomPGReplacement *replacement;
	Path *fallback;
	Path *partial_fallback = NULL;
	CustomPath *path;
	double rows;
	double original_rows;
	ListCell *cell;
	List *native_paths;
	List *native_partial_paths;
	List *equivalent_parameterized_paths = NIL;
	uint64 equivalent_parameterized_count = 0;
	uint64 retained_parameterized_paths = 0;

	if (previous_set_rel_pathlist_hook != NULL)
		previous_set_rel_pathlist_hook(root, rel, rti, rte);
	if (active_replacements == NIL || root->parent_root != NULL ||
		rel->reloptkind != RELOPT_BASEREL ||
		(rte->rtekind != RTE_RELATION && rte->rtekind != RTE_CTE &&
		 rte->rtekind != RTE_SUBQUERY))
		return;
	replacement = bloompg_find_replacement(rti);
	if (replacement == NULL)
		return;
	if ((replacement->kind == BLOOMPG_REPLACEMENT_MEMORY ||
		 replacement->kind == BLOOMPG_REPLACEMENT_EMPTY) &&
		!bloompg_targetlist_compatible(rel->reltarget,
									 replacement->targetlist))
	{
		if (bloompg_log_transfer_steps)
			elog(LOG, "bloompg P1 skipped RTI %u: target width %d differs from materialization width %d",
				 rti, list_length(rel->reltarget->exprs),
				 list_length(replacement->targetlist));
		return;
	}
	if (replacement->kind == BLOOMPG_REPLACEMENT_FILTER)
	{
		(void) bloompg_transform_filter_pathset(rel, rti, replacement);
		return;
	}
	fallback = bloompg_fallback_path(root, rel, rte);
	if (fallback == NULL)
		return;
	if (rel->consider_parallel && replacement->parallel_dsm_allowed &&
		replacement->kind != BLOOMPG_REPLACEMENT_EMPTY)
	{
		foreach(cell, rel->partial_pathlist)
		{
			Path *candidate = lfirst(cell);

			if (candidate->param_info != NULL || !candidate->parallel_safe ||
				candidate->parallel_workers <= 0)
				continue;
			if (partial_fallback == NULL ||
				candidate->total_cost < partial_fallback->total_cost)
				partial_fallback = candidate;
		}
	}

	/* Memory/TID/Empty are true relation replacements.  Detach the native
	 * PathSet before exposing the replacement; selected native nodes remain
	 * reachable only as executor safety children. */
	native_paths = rel->pathlist;
	native_partial_paths = rel->partial_pathlist;
	if (replacement->native_parameterized_paths_equivalent &&
		(replacement->kind == BLOOMPG_REPLACEMENT_MEMORY ||
		 replacement->kind == BLOOMPG_REPLACEMENT_TID))
	{
		foreach(cell, native_paths)
		{
			Path *candidate = lfirst(cell);

			if (candidate->param_info != NULL)
				equivalent_parameterized_paths =
					lappend(equivalent_parameterized_paths, candidate);
		}
	}
	equivalent_parameterized_count = list_length(
		equivalent_parameterized_paths);
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;

	original_rows = Max(rel->rows, 1.0);
	rows = Max(replacement->rows, 1.0);
	rel->rows = rows;
	if (replacement->kind == BLOOMPG_REPLACEMENT_MEMORY &&
		replacement->statistics != NULL)
	{
		rel->tuples = rows;
		if (replacement->statistics->average_width > 0)
			rel->reltarget->width = replacement->statistics->average_width;
	}
	path = bloompg_replacement_path(replacement, rti, rel, fallback, rows,
		false, 0);
	add_path(rel, &path->path);
	/* A local-qual-only materialization and its native parameterized paths
	 * produce the same logical rows.  Keep those keyed alternatives beside the
	 * replacement so PostgreSQL can choose between one sequential vector read
	 * and repeated index probes.  A materialization that consumed an incoming
	 * Bloom filter is not equivalent and therefore remains a true replacement. */
	foreach(cell, equivalent_parameterized_paths)
		add_path(rel, (Path *) lfirst(cell));
	if (equivalent_parameterized_count > 0)
	{
		foreach(cell, rel->pathlist)
		{
			Path *candidate = lfirst(cell);

			if (candidate->param_info != NULL)
				retained_parameterized_paths++;
		}
		bloompg_path_transform_record_replacement_parameterized(
			equivalent_parameterized_count,
			retained_parameterized_paths);
	}
	if (partial_fallback != NULL)
	{
		double divisor = Max(original_rows /
			Max(partial_fallback->rows, 1.0), 1.0);
		double partial_rows = Max(rows / divisor, 1.0);
		CustomPath *partial = bloompg_replacement_path(replacement, rti, rel,
			partial_fallback, partial_rows, true,
			partial_fallback->parallel_workers);

		add_partial_path(rel, &partial->path);
	}
	list_free(native_paths);
	list_free(native_partial_paths);
	list_free(equivalent_parameterized_paths);
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg P1 added %s path for RTI %u with %.0f rows%s%s",
			replacement->kind == BLOOMPG_REPLACEMENT_TID ? "TID" :
			replacement->kind == BLOOMPG_REPLACEMENT_EMPTY ? "empty" :
			"memory",
			rti, rows, partial_fallback != NULL ? " plus parallel path" : "",
			equivalent_parameterized_count > 0 ?
			" plus equivalent parameterized paths" : "");
}

static Plan *
bloompg_plan_tid_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  CustomPath *best_path,
					  List *targetlist,
					  List *clauses,
					  List *custom_plans)
{
	Index rti;
	uint64 token;
	uint64 rows;

	(void) root;
	(void) rel;
	(void) clauses;
	if (list_length(best_path->custom_private) != 3 ||
		list_length(custom_plans) != 1)
		elog(ERROR, "invalid bloompg TID path metadata");
	rti = (Index) intVal(list_nth(best_path->custom_private, 0));
	token = strtoull(strVal(list_nth(best_path->custom_private, 1)), NULL, 10);
	rows = strtoull(strVal(list_nth(best_path->custom_private, 2)), NULL, 10);
	return bloompg_make_tid_scan_path(targetlist,
		(Plan *) linitial(custom_plans), rti, token, rows);
}

static Plan *
bloompg_plan_memory_path(PlannerInfo *root,
						 RelOptInfo *rel,
						 CustomPath *best_path,
						 List *targetlist,
						 List *clauses,
						 List *custom_plans)
{
	uint64 token;
	uint64 rows;

	(void) root;
	(void) rel;
	(void) clauses;
	if (list_length(best_path->custom_private) != 4 ||
		list_length(custom_plans) != 1)
		elog(ERROR, "invalid bloompg memory path metadata");
	token = strtoull(strVal(list_nth(best_path->custom_private, 1)), NULL, 10);
	rows = strtoull(strVal(list_nth(best_path->custom_private, 2)), NULL, 10);
	return bloompg_make_memory_scan_path(targetlist,
										 (List *) list_nth(best_path->custom_private, 3),
										 (Plan *) linitial(custom_plans),
										 token, rows);
}

static Plan *
bloompg_plan_empty_path(PlannerInfo *root,
						RelOptInfo *rel,
						CustomPath *best_path,
						List *targetlist,
						List *clauses,
						List *custom_plans)
{
	uint64 token;
	Plan *fallback_plan;

	(void) root;
	(void) rel;
	(void) clauses;
	if (list_length(best_path->custom_private) != 3 ||
		list_length(custom_plans) != 1)
		elog(ERROR, "invalid bloompg empty path metadata");
	token = strtoull(strVal(list_nth(best_path->custom_private, 1)), NULL, 10);
	fallback_plan = (Plan *) linitial(custom_plans);
	return bloompg_make_empty_scan_path(targetlist,
		(List *) list_nth(best_path->custom_private, 2),
		fallback_plan, token);
}

void
bloompg_replan_begin(List *replacements, const BloomPGJoinGraph *graph)
{
	Assert(active_replacements == NIL);
	bloompg_path_transform_reset();
	active_replacements = replacements;
	active_graph = graph;
	cardinality_adjustments = 0;
	cardinality_max_ratio = 1.0;
}

void
bloompg_replan_end(void)
{
	active_replacements = NIL;
	active_graph = NULL;
}

void
bloompg_replan_cardinality_stats(uint64 *adjustments, double *max_ratio)
{
	if (adjustments != NULL)
		*adjustments = cardinality_adjustments;
	if (max_ratio != NULL)
		*max_ratio = cardinality_max_ratio;
}

void
bloompg_replan_init(void)
{
	previous_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = bloompg_set_rel_pathlist;
	previous_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = bloompg_set_join_pathlist;
}

void
bloompg_replan_fini(void)
{
	if (set_rel_pathlist_hook == bloompg_set_rel_pathlist)
		set_rel_pathlist_hook = previous_set_rel_pathlist_hook;
	if (set_join_pathlist_hook == bloompg_set_join_pathlist)
		set_join_pathlist_hook = previous_set_join_pathlist_hook;
	active_replacements = NIL;
	active_graph = NULL;
}
