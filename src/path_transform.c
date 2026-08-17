#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/scan.h"
#include "bloompg/path_transform.h"
#include "bloompg/sampling.h"

#include "catalog/pg_proc.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "utils/lsyscache.h"

/*
 * FILTER is deliberately implemented outside the relation-replacement
 * machinery:
 *
 *     Filter(PathSet) = { Filter(path) | path in PathSet }
 *
 * The wrapped native Path remains the executable child.  In particular, an
 * IndexPath retains its index clauses and ParamPathInfo, a BitmapHeapPath
 * retains its bitmap tree, and a partial path retains its parallel contract.
 */

typedef struct BloomPGFilterCandidate
{
	Path	   *fallback;
	List	   *metadata;
	bool		partial;
} BloomPGFilterCandidate;

typedef struct BloomPGFilterPathCounts
{
	int			seq;
	int			index;
	int			index_only;
	int			bitmap;
	int			other;
	int			parameterized;
	int			partial;
} BloomPGFilterPathCounts;

typedef struct BloomPGPathTransformTotals
{
	uint64		relations;
	uint64		input_paths;
	uint64		wrapped_paths;
	uint64		retained_paths;
	uint64		seq;
	uint64		index;
	uint64		index_only;
	uint64		bitmap;
	uint64		other;
	uint64		parameterized;
	uint64		partial;
	uint64		replacement_parameterized_input;
	uint64		replacement_parameterized_retained;
} BloomPGPathTransformTotals;

static BloomPGPathTransformTotals transform_totals;

static Plan *bloompg_plan_filter_path(PlannerInfo *root,
	RelOptInfo *rel, CustomPath *best_path, List *targetlist, List *clauses,
	List *custom_plans);

static const CustomPathMethods bloompg_filter_path_methods = {
	.CustomName = "BloomPG Filter",
	.PlanCustomPath = bloompg_plan_filter_path
};

static AttrNumber
bloompg_path_target_attribute(PathTarget *target, Index rti,
	AttrNumber base_attribute)
{
	ListCell   *cell;
	AttrNumber output_attribute = 1;

	foreach(cell, target->exprs)
	{
		Node *expression = strip_implicit_coercions((Node *) lfirst(cell));

		if (IsA(expression, Var))
		{
			Var *var = castNode(Var, expression);

			if ((((Index) var->varno == rti &&
				  var->varattno == base_attribute) ||
				 ((Index) var->varnosyn == rti &&
				  var->varattnosyn == base_attribute)))
				return output_attribute;
		}
		output_attribute++;
	}
	return InvalidAttrNumber;
}

static List *
bloompg_filter_path_metadata(PathTarget *target, Index rti, List *filters,
	bool *mapped)
{
	List	   *result = NIL;
	ListCell   *cell;

	*mapped = false;
	if (target == NULL)
		return NIL;
	foreach(cell, filters)
	{
		BloomPGSampleFilter *filter = lfirst(cell);
		List	   *attributes = NIL;
		int			key;

		for (key = 0; key < filter->nkeys; key++)
		{
			AttrNumber attribute = bloompg_path_target_attribute(target, rti,
				filter->attributes[key]);

			if (!AttributeNumberIsValid(attribute))
				return NIL;
			attributes = lappend_int(attributes, attribute);
		}
		result = lappend(result, list_make3(
			makeString(psprintf(UINT64_FORMAT, filter->token)),
			makeInteger(filter->nkeys), attributes));
	}
	*mapped = result != NIL;
	return result;
}

static bool
bloompg_filters_parallel_safe(List *filters)
{
	ListCell *cell;

	foreach(cell, filters)
	{
		BloomPGSampleFilter *sample_filter = lfirst(cell);
		BloomPGFilter *filter = sample_filter->filter;
		int			key;

		if (filter == NULL || !filter->valid)
			return false;
		for (key = 0; key < filter->nkeys; key++)
		{
			if (!OidIsValid(filter->keys[key].probe_hash_proc) ||
				func_parallel(filter->keys[key].probe_hash_proc) !=
				PROPARALLEL_SAFE)
				return false;
		}
	}
	return true;
}

static Cost
bloompg_filter_tuple_cost(List *filters)
{
	int			operations = 0;
	ListCell   *cell;

	foreach(cell, filters)
	{
		BloomPGSampleFilter *filter = lfirst(cell);

		/* One membership decision plus one hash/equality input per key. */
		operations += 1 + Max(filter->nkeys, 1);
	}
	return cpu_operator_cost * Max(operations, 1);
}

static List *
bloompg_collect_filter_candidates(List *paths, Index rti, List *filters,
	bool partial, bool *mapped)
{
	List	   *result = NIL;
	ListCell   *cell;

	*mapped = true;
	foreach(cell, paths)
	{
		Path *fallback = lfirst(cell);
		BloomPGFilterCandidate *candidate;
		bool target_mapped;
		List *metadata = bloompg_filter_path_metadata(fallback->pathtarget,
			rti, filters, &target_mapped);

		if (!target_mapped)
		{
			*mapped = false;
			return NIL;
		}
		candidate = palloc0(sizeof(BloomPGFilterCandidate));
		candidate->fallback = fallback;
		candidate->metadata = metadata;
		candidate->partial = partial;
		result = lappend(result, candidate);
	}
	return result;
}

static void
bloompg_count_path(BloomPGFilterPathCounts *counts, Path *path, bool partial)
{
	if (path->param_info != NULL)
		counts->parameterized++;
	if (partial)
		counts->partial++;
	switch (path->pathtype)
	{
		case T_SeqScan:
		case T_SampleScan:
			counts->seq++;
			break;
		case T_IndexScan:
			counts->index++;
			break;
		case T_IndexOnlyScan:
			counts->index_only++;
			break;
		case T_BitmapHeapScan:
			counts->bitmap++;
			break;
		default:
			counts->other++;
			break;
	}
}

void
bloompg_path_transform_reset(void)
{
	MemSet(&transform_totals, 0, sizeof(transform_totals));
}

void
bloompg_path_transform_record_replacement_parameterized(uint64 input_paths,
	uint64 retained_paths)
{
	transform_totals.replacement_parameterized_input += input_paths;
	transform_totals.replacement_parameterized_retained += retained_paths;
}

char *
bloompg_path_transform_json(void)
{
	return psprintf(
		"{\"relations\":" UINT64_FORMAT
		",\"input_paths\":" UINT64_FORMAT
		",\"wrapped_paths\":" UINT64_FORMAT
		",\"retained_paths\":" UINT64_FORMAT
		",\"seq\":" UINT64_FORMAT
		",\"index\":" UINT64_FORMAT
		",\"index_only\":" UINT64_FORMAT
		",\"bitmap\":" UINT64_FORMAT
		",\"other\":" UINT64_FORMAT
		",\"parameterized\":" UINT64_FORMAT
		",\"partial\":" UINT64_FORMAT
		",\"replacement_parameterized_input\":" UINT64_FORMAT
		",\"replacement_parameterized_retained\":" UINT64_FORMAT "}",
		transform_totals.relations, transform_totals.input_paths,
		transform_totals.wrapped_paths, transform_totals.retained_paths,
		transform_totals.seq, transform_totals.index,
		transform_totals.index_only, transform_totals.bitmap,
		transform_totals.other, transform_totals.parameterized,
		transform_totals.partial,
		transform_totals.replacement_parameterized_input,
		transform_totals.replacement_parameterized_retained);
}

static CustomPath *
bloompg_make_filter_path(RelOptInfo *rel, Index rti,
	BloomPGFilterCandidate *candidate, Selectivity selectivity,
	Cost tuple_filter_cost, bool filters_parallel_safe)
{
	Path	   *fallback = candidate->fallback;
	CustomPath *path = makeNode(CustomPath);
	double		rows = clamp_row_est(fallback->rows * selectivity);

	path->path.pathtype = T_CustomScan;
	path->path.parent = rel;
	path->path.pathtarget = fallback->pathtarget;
	path->path.param_info = fallback->param_info;
	path->path.parallel_aware = fallback->parallel_aware;
	path->path.parallel_safe = fallback->parallel_safe &&
		filters_parallel_safe;
	path->path.parallel_workers = fallback->parallel_workers;
	path->path.rows = rows;
	path->path.disabled_nodes = fallback->disabled_nodes;
	path->path.startup_cost = fallback->startup_cost;
	path->path.total_cost = fallback->total_cost +
		Max(fallback->rows, 0.0) * tuple_filter_cost;
	path->path.pathkeys = fallback->pathkeys;

	/* Projection is implemented by the wrapper.  Backward scan and
	 * mark/restore are intentionally not claimed for every access method;
	 * PostgreSQL can add Material when an upper node requires them. */
	path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	path->custom_paths = list_make1(fallback);
	path->custom_restrictinfo = NIL;
	path->custom_private = list_make2(makeInteger((int) rti),
		candidate->metadata);
	path->methods = &bloompg_filter_path_methods;
	return path;
}

bool
bloompg_transform_filter_pathset(RelOptInfo *rel, Index rti,
	const BloomPGReplacement *replacement)
{
	List	   *candidates;
	List	   *partial_candidates = NIL;
	List	   *native_paths;
	List	   *native_partial_paths;
	ListCell   *cell;
	double		original_rows;
	double		filtered_rows;
	Selectivity selectivity;
	Cost		tuple_filter_cost;
	bool		mapped;
	bool		partial_mapped = true;
	bool		parallel_safe;
	BloomPGFilterPathCounts counts = {0};

	if (replacement == NULL || replacement->filters == NIL ||
		rel->pathlist == NIL)
		return false;

	/* Validate every target before changing either PathSet.  A path-specific
	 * target that omits a probe key cannot be filtered positionally.  In that
	 * rare case retain the complete native PathSet instead of applying a
	 * partial transform with inconsistent relation cardinality. */
	candidates = bloompg_collect_filter_candidates(rel->pathlist, rti,
		replacement->filters, false, &mapped);
	if (!mapped || candidates == NIL)
	{
		if (bloompg_log_transfer_steps)
			elog(LOG, "bloompg P1 skipped Filter(PathSet) for RTI %u: a native path target omits a Bloom key",
				rti);
		return false;
	}
	parallel_safe = bloompg_filters_parallel_safe(replacement->filters);
	if (rel->partial_pathlist != NIL && parallel_safe &&
		replacement->parallel_dsm_allowed)
	{
		partial_candidates = bloompg_collect_filter_candidates(
			rel->partial_pathlist, rti, replacement->filters, true,
			&partial_mapped);
		if (!partial_mapped)
		{
			if (bloompg_log_transfer_steps)
				elog(LOG, "bloompg P1 skipped Filter(PathSet) for RTI %u: a partial path target omits a Bloom key",
					rti);
			return false;
		}
	}

	original_rows = Max(rel->rows, 1.0);
	filtered_rows = clamp_row_est(Min(Max(replacement->rows, 0.0),
		original_rows));
	selectivity = Min(Max(filtered_rows / original_rows, 0.0), 1.0);
	tuple_filter_cost = bloompg_filter_tuple_cost(replacement->filters);

	/* Detach the native lists without freeing their Path nodes: each node is
	 * now owned as the executable child of exactly one wrapper.  This avoids
	 * type-specific Path copying and therefore also preserves paths supplied
	 * by other extensions.  add_path() only ever prunes wrapper roots. */
	native_paths = rel->pathlist;
	native_partial_paths = rel->partial_pathlist;
	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;
	rel->rows = filtered_rows;

	foreach(cell, candidates)
	{
		BloomPGFilterCandidate *candidate = lfirst(cell);
		CustomPath *path = bloompg_make_filter_path(rel, rti, candidate,
			selectivity, tuple_filter_cost, parallel_safe);

		bloompg_count_path(&counts, candidate->fallback, false);
		add_path(rel, &path->path);
	}
	foreach(cell, partial_candidates)
	{
		BloomPGFilterCandidate *candidate = lfirst(cell);
		CustomPath *path = bloompg_make_filter_path(rel, rti, candidate,
			selectivity, tuple_filter_cost, parallel_safe);

		bloompg_count_path(&counts, candidate->fallback, true);
		add_partial_path(rel, &path->path);
	}
	transform_totals.relations++;
	transform_totals.input_paths += list_length(native_paths) +
		list_length(native_partial_paths);
	transform_totals.wrapped_paths += list_length(candidates) +
		list_length(partial_candidates);
	transform_totals.retained_paths += list_length(rel->pathlist) +
		list_length(rel->partial_pathlist);
	transform_totals.seq += counts.seq;
	transform_totals.index += counts.index;
	transform_totals.index_only += counts.index_only;
	transform_totals.bitmap += counts.bitmap;
	transform_totals.other += counts.other;
	transform_totals.parameterized += counts.parameterized;
	transform_totals.partial += counts.partial;

	/* Only list cells are obsolete.  The native Path objects remain reachable
	 * through CustomPath.custom_paths. */
	list_free(native_paths);
	list_free(native_partial_paths);

	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg P1 Filter(PathSet) RTI %u: seq=%d index=%d index_only=%d bitmap=%d other=%d parameterized=%d partial=%d rows=%.0f selectivity=%.6f%s",
			rti, counts.seq, counts.index, counts.index_only, counts.bitmap,
			counts.other, counts.parameterized, counts.partial, filtered_rows,
			selectivity, parallel_safe ? "" : " parallel-filter-unsafe");
	return true;
}

static Plan *
bloompg_plan_filter_path(PlannerInfo *root, RelOptInfo *rel,
	CustomPath *best_path, List *targetlist, List *clauses,
	List *custom_plans)
{
	Plan	   *fallback_plan;
	CustomScan *scan;
	List	   *metadata;

	(void) root;
	(void) clauses;
	if (list_length(best_path->custom_private) != 2 ||
		list_length(custom_plans) != 1)
		elog(ERROR, "invalid bloompg filter path metadata");
	fallback_plan = (Plan *) linitial(custom_plans);
	metadata = (List *) list_nth(best_path->custom_private, 1);
	scan = castNode(CustomScan, bloompg_make_filter_scan_path(targetlist,
		fallback_plan->targetlist, fallback_plan, metadata));
	scan->flags = best_path->flags;
	scan->custom_relids = bms_copy(rel->relids);
	return &scan->scan.plan;
}
