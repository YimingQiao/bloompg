#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/scan.h"
#include "bloompg/path_transform.h"
#include "bloompg/replan.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_statistic.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

static set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = NULL;
static get_relation_stats_hook_type previous_get_relation_stats_hook = NULL;
static Bitmapset *active_statistics_relids = NULL;
static MemoryContext active_replan_context = NULL;
static List *active_replacements = NIL;
static const BloomPGJoinGraph *active_graph = NULL;

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

/*
 * Supply query-local pg_statistic tuples, not join selectivity rules.  Enable
 * them only after replacing the base path: original scan quals must still be
 * costed against the original relation, rather than filtering its survivors
 * a second time.  RTIs, not relation OIDs, distinguish self-join aliases.
 */
static bool
bloompg_get_relation_stats(PlannerInfo *root, RangeTblEntry *rte,
	AttrNumber attnum, VariableStatData *vardata)
{
	BloomPGReplacement *replacement;
	BloomPGMaterializedStatistics *statistics;
	ListCell *cell;
	int rti = -1;

	if (active_replacements == NIL || root->parent_root != NULL ||
		rte->rtekind != RTE_RELATION || attnum <= 0)
		goto fallback;
	/* Some callers (e.g., BRIN costing) leave vardata's input fields unset. */
	while ((rti = bms_next_member(active_statistics_relids, rti)) >= 0)
	{
		if (rti < root->simple_rel_array_size &&
			root->simple_rte_array[rti] == rte)
			break;
	}
	if (rti < 0)
		goto fallback;
	replacement = bloompg_find_replacement(rti);
	if (replacement == NULL || replacement->statistics == NULL)
		goto fallback;
	statistics = replacement->statistics;
	if (statistics->rows == 0)
		goto fallback;
	foreach(cell, statistics->join_keys)
	{
		BloomPGJoinKeyStatistics *keys = lfirst(cell);
		Datum values[Natts_pg_statistic] = {0};
		bool nulls[Natts_pg_statistic] = {false};
		Relation catalog;
		Bitmapset *attributes;
		Oid typid;
		Oid collid;
		int32 typmod;
		int column;

		/* A composite NDV is not the NDV of any constituent column. */
		if (keys->nkeys != 1 || keys->base_attributes[0] != attnum)
			continue;
		get_atttypetypmodcoll(rte->relid, attnum, &typid, &typmod, &collid);
		values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(rte->relid);
		values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(attnum);
		values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(rte->inh);
		values[Anum_pg_statistic_stanullfrac - 1] =
			Float4GetDatum((float4) ((double) keys->null_rows / statistics->rows));
		values[Anum_pg_statistic_stadistinct - 1] =
			Float4GetDatum((float4) Min(keys->ndistinct, (double) keys->nonnull_rows));
		values[Anum_pg_statistic_stawidth - 1] =
			Int32GetDatum(get_typavgwidth(typid, typmod));
		/* No post-filter MCVs or histograms were collected.  Do not attach
		 * the original distribution to the new NDV and NULL fraction. */
		for (column = Anum_pg_statistic_stanumbers1 - 1;
			 column < Natts_pg_statistic; column++)
			nulls[column] = true;
		catalog = table_open(StatisticRelationId, AccessShareLock);
		vardata->statsTuple = heap_form_tuple(RelationGetDescr(catalog),
			values, nulls);
		table_close(catalog, AccessShareLock);
		vardata->freefunc = heap_freetuple;
		attributes = bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber);
		vardata->acl_ok = all_rows_selectable(root, rti, attributes);
		bms_free(attributes);
		return true;
	}

fallback:
	return previous_get_relation_stats_hook != NULL &&
		previous_get_relation_stats_hook(root, rte, attnum, vardata);
}

static bool
bloompg_replacement_is_exact(Index rti)
{
	BloomPGReplacement *replacement = bloompg_find_replacement(rti);

	return replacement != NULL && replacement->rows > 0.0 &&
		(replacement->kind == BLOOMPG_REPLACEMENT_MEMORY ||
		 replacement->kind == BLOOMPG_REPLACEMENT_TID);
}

/* A materialized relation and its native parameterized index path are
 * interchangeable in a join only when every filter consumed by the
 * materialization came directly from an exact relation that parameterizes
 * the native path.  The join clauses then enforce the same key domains while
 * retaining PostgreSQL's efficient keyed-rescan implementation. */
static bool
bloompg_parameterized_path_is_equivalent(BloomPGReplacement *replacement,
	Path *path)
{
	int source_rti = -1;

	if (path->param_info == NULL)
		return false;
	if (replacement->native_parameterized_paths_equivalent)
		return true;
	if (replacement->materialization_filter_sources == NULL)
		return false;
	while ((source_rti = bms_next_member(
		replacement->materialization_filter_sources, source_rti)) >= 0)
	{
		if (!bms_is_member(source_rti, path->param_info->ppi_req_outer) ||
			!bloompg_replacement_is_exact(source_rti))
			return false;
	}
	return true;
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
	if (replacement->kind == BLOOMPG_REPLACEMENT_MEMORY ||
		replacement->kind == BLOOMPG_REPLACEMENT_TID)
	{
		foreach(cell, native_paths)
		{
			Path *candidate = lfirst(cell);

			if (bloompg_parameterized_path_is_equivalent(replacement,
					candidate))
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
	/* Supply reduced base cardinalities; leave join estimation to PostgreSQL. */
	rel->rows = rows;
	if ((replacement->kind == BLOOMPG_REPLACEMENT_MEMORY ||
		 replacement->kind == BLOOMPG_REPLACEMENT_TID) &&
		replacement->statistics != NULL)
	{
		rel->tuples = rows;
		if (replacement->kind == BLOOMPG_REPLACEMENT_MEMORY &&
			replacement->statistics->average_width > 0)
			rel->reltarget->width = replacement->statistics->average_width;
	}
	path = bloompg_replacement_path(replacement, rti, rel, fallback, rows,
		false, 0);
	/* A large materialization whose exact incoming domain can also be enforced
	 * by a parameterized index is a poor inner-rescan choice: a single bad
	 * cardinality estimate can multiply a full vector scan thousands of times.
	 * Soft-disable that unparameterized alternative.  PostgreSQL may still use
	 * it to break a parameterization cycle, but otherwise prefers the keyed
	 * path that already enforces the same domain. */
	if (replacement->materialization_filter_sources != NULL &&
		rows > (double) bloompg_index_guard_min_rows &&
		equivalent_parameterized_paths != NIL)
		path->path.disabled_nodes = 1;
	add_path(rel, &path->path);
	/* Keep only native parameterized paths that enforce every exact domain
	 * consumed by the materialization.  PostgreSQL can then choose between one
	 * sequential vector read and logically equivalent repeated index probes. */
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
		int parallel_workers = Max(partial_fallback->parallel_workers,
			Min(bloompg_transfer_workers,
				max_parallel_workers_per_gather));
		double divisor = Max(original_rows /
			Max(partial_fallback->rows, 1.0), 1.0);
		double partial_rows;
		CustomPath *partial;

		if (parallel_workers > partial_fallback->parallel_workers)
			divisor *= (double) parallel_workers /
				(double) partial_fallback->parallel_workers;
		partial_rows = Max(rows / divisor, 1.0);
		partial = bloompg_replacement_path(replacement, rti, rel,
			partial_fallback, partial_rows, true,
			parallel_workers);

		add_partial_path(rel, &partial->path);
	}
	if (replacement->statistics != NULL &&
		(replacement->kind == BLOOMPG_REPLACEMENT_MEMORY ||
		 replacement->kind == BLOOMPG_REPLACEMENT_TID))
	{
		MemoryContext old_context = MemoryContextSwitchTo(active_replan_context);

		active_statistics_relids = bms_add_member(active_statistics_relids, rti);
		MemoryContextSwitchTo(old_context);
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
			psprintf(" plus " UINT64_FORMAT "/" UINT64_FORMAT
				" equivalent parameterized paths",
				equivalent_parameterized_count,
				retained_parameterized_paths) : "");
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
	Assert(active_statistics_relids == NULL);
	bloompg_path_transform_reset();
	active_replan_context = CurrentMemoryContext;
	active_replacements = replacements;
	active_graph = graph;
}

void
bloompg_replan_end(void)
{
	active_replacements = NIL;
	active_graph = NULL;
	bms_free(active_statistics_relids);
	active_statistics_relids = NULL;
	active_replan_context = NULL;
}

void
bloompg_replan_init(void)
{
	previous_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = bloompg_set_rel_pathlist;
	previous_get_relation_stats_hook = get_relation_stats_hook;
	get_relation_stats_hook = bloompg_get_relation_stats;
}

void
bloompg_replan_fini(void)
{
	if (set_rel_pathlist_hook == bloompg_set_rel_pathlist)
		set_rel_pathlist_hook = previous_set_rel_pathlist_hook;
	if (get_relation_stats_hook == bloompg_get_relation_stats)
		get_relation_stats_hook = previous_get_relation_stats_hook;
	active_replacements = NIL;
	active_graph = NULL;
	bms_free(active_statistics_relids);
	active_statistics_relids = NULL;
	active_replan_context = NULL;
}
