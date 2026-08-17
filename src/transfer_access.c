#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/filter.h"
#include "bloompg/planner.h"
#include "bloompg/scan.h"
#include "bloompg/scan_quals.h"
#include "bloompg/transfer_internal.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "port/pg_bitutils.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

/*
 * Transfer access planning
 * ------------------------
 *
 * P0 answers a logical question: which independent base-relation operator can
 * Bloom use while analysing the join graph?  It is not necessarily a sensible
 * physical path after incoming exact key sets have been built.  This module
 * turns the current transfer predicates into a private, single-relation Query
 * and asks PostgreSQL to cost the access paths again.  Core can consequently
 * combine the original local predicates with exact transfer predicates and
 * choose IndexScan, IndexOnlyScan, BitmapHeap/BitmapAnd, or a parallel scan.
 *
 * The generated predicates are access hints only in the semantic sense: the
 * executor wrapper rechecks every registered transfer filter before a tuple is
 * appended to the column store.  Approximate Bloom filters are never promoted
 * into exact index conditions.
 */

typedef struct BloomPGAccessExpressionContext
{
	Index		rti;
	bool		invalid;
} BloomPGAccessExpressionContext;

typedef struct BloomPGAccessPlanShape
{
	uint64		seq_scans;
	uint64		index_scans;
	uint64		index_only_scans;
	uint64		bitmap_heap_scans;
	uint64		bitmap_index_scans;
	uint64		gathers;
} BloomPGAccessPlanShape;

typedef struct BloomPGAccessExactCandidate
{
	BloomPGSampleFilter *sample_filter;
	uint64		keys;
	double		fraction;
} BloomPGAccessExactCandidate;

static bool
bloompg_access_expression_walker(Node *node, void *opaque)
{
	BloomPGAccessExpressionContext *context = opaque;

	if (node == NULL || context->invalid)
		return false;
	if (IsA(node, Var))
	{
		Var *var = castNode(Var, node);

		if (var->varlevelsup != 0 || (Index) var->varno != context->rti)
			context->invalid = true;
		return false;
	}
	if (IsA(node, Param))
	{
		if (castNode(Param, node)->paramkind != PARAM_EXTERN)
			context->invalid = true;
		return false;
	}
	if (IsA(node, SubPlan) || IsA(node, AlternativeSubPlan) ||
		IsA(node, PlaceHolderVar))
	{
		context->invalid = true;
		return false;
	}
	return expression_tree_walker(node, bloompg_access_expression_walker,
		context);
}

static bool
bloompg_access_expressions_safe(Node *node, Index rti)
{
	BloomPGAccessExpressionContext context = {.rti = rti};

	(void) bloompg_access_expression_walker(node, &context);
	return !context.invalid;
}

static uint64
bloompg_bitmap_distinct_keys(const BloomPGFilter *filter)
{
	uint64		distinct = 0;
	uint64		word_count = filter->nbits / 64;
	uint64		word;

	for (word = 0; word < word_count; word++)
		distinct += (uint64) pg_popcount64(filter->words[word]);
	return distinct;
}

static bool
bloompg_integral_const_datum(int64 value, Oid type_oid, Datum *result)
{
	switch (type_oid)
	{
		case INT2OID:
			if (value < PG_INT16_MIN || value > PG_INT16_MAX)
				return false;
			*result = Int16GetDatum((int16) value);
			return true;
		case INT4OID:
			if (value < PG_INT32_MIN || value > PG_INT32_MAX)
				return false;
			*result = Int32GetDatum((int32) value);
			return true;
		case INT8OID:
			*result = Int64GetDatum(value);
			return true;
		case OIDOID:
			if (value < 0 || (uint64) value > UINT32_MAX)
				return false;
			*result = ObjectIdGetDatum((Oid) value);
			return true;
		case DATEOID:
			if (value < PG_INT32_MIN || value > PG_INT32_MAX)
				return false;
			*result = DateADTGetDatum((DateADT) value);
			return true;
		default:
			return false;
	}
}

static ScalarArrayOpExpr *
bloompg_make_exact_array_qual(BloomPGRelationState *state,
	BloomPGSampleFilter *sample_filter, uint64 *key_count)
{
	BloomPGFilter *filter = sample_filter->filter;
	TypeCacheEntry *type_cache;
	List	   *constants = NIL;
	Var		   *variable;
	Oid			attribute_type;
	Oid			attribute_collation;
	int32		attribute_typmod;
	int16		type_length;
	bool		type_by_value;
	uint64		word_count;
	uint64		word_index;
	uint64		distinct;

	*key_count = 0;
	if (filter == NULL || !filter->valid ||
		filter->kind != BLOOMPG_FILTER_BITMAP || filter->nkeys != 1 ||
		sample_filter->nkeys != 1 || sample_filter->attributes == NULL ||
		!AttributeNumberIsValid(sample_filter->attributes[0]) ||
		filter->keys[0].build_type != filter->keys[0].probe_type)
		return NULL;
	distinct = bloompg_bitmap_distinct_keys(filter);
	if (distinct == 0 ||
		distinct > (uint64) bloompg_index_transfer_max_keys ||
		distinct > (uint64) INT_MAX)
		return NULL;
	get_atttypetypmodcoll(state->table->relation_oid,
		sample_filter->attributes[0], &attribute_type, &attribute_typmod,
		&attribute_collation);
	if (attribute_type != filter->keys[0].probe_type)
		return NULL;
	type_cache = lookup_type_cache(attribute_type, TYPECACHE_EQ_OPR);
	if (type_cache == NULL || !OidIsValid(type_cache->eq_opr))
		return NULL;
	get_typlenbyval(attribute_type, &type_length, &type_by_value);
	word_count = filter->nbits / 64;
	for (word_index = 0; word_index < word_count; word_index++)
	{
		uint64 word = filter->words[word_index];

		while (word != 0)
		{
			int bit = pg_rightmost_one_pos64(word);
			uint64 offset = word_index * 64 + (uint64) bit;
			int64 value = filter->bitmap_lower + (int64) offset;
			Datum datum;

			word &= word - 1;
			if (value > filter->bitmap_upper ||
				!bloompg_integral_const_datum(value, attribute_type, &datum))
				continue;
			constants = lappend(constants, makeConst(attribute_type, -1,
				attribute_collation, type_length, datum, false, type_by_value));
		}
	}
	if ((uint64) list_length(constants) != distinct)
	{
		list_free_deep(constants);
		return NULL;
	}
	variable = makeVar(state->rti, sample_filter->attributes[0],
		attribute_type, attribute_typmod, attribute_collation, 0);
	*key_count = distinct;
	return make_SAOP_expr(type_cache->eq_opr, (Node *) variable,
		attribute_type, attribute_collation, attribute_collation, constants,
		false);
}

static int
bloompg_compare_exact_candidates(const void *left, const void *right)
{
	const BloomPGAccessExactCandidate *a = left;
	const BloomPGAccessExactCandidate *b = right;

	if (a->fraction < b->fraction)
		return -1;
	if (a->fraction > b->fraction)
		return 1;
	if (a->keys < b->keys)
		return -1;
	if (a->keys > b->keys)
		return 1;
	if (a->sample_filter->token < b->sample_filter->token)
		return -1;
	if (a->sample_filter->token > b->sample_filter->token)
		return 1;
	return 0;
}

static void
bloompg_access_plan_shape(Plan *plan, BloomPGAccessPlanShape *shape)
{
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return;
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			shape->seq_scans++;
			break;
		case T_IndexScan:
			shape->index_scans++;
			break;
		case T_IndexOnlyScan:
			shape->index_only_scans++;
			break;
		case T_BitmapHeapScan:
			shape->bitmap_heap_scans++;
			break;
		case T_BitmapIndexScan:
			shape->bitmap_index_scans++;
			break;
		case T_Gather:
		case T_GatherMerge:
			shape->gathers++;
			break;
		default:
			break;
	}
	bloompg_access_plan_shape(outerPlan(plan), shape);
	bloompg_access_plan_shape(innerPlan(plan), shape);
	switch (nodeTag(plan))
	{
		case T_Append:
			children = castNode(Append, plan)->appendplans;
			break;
		case T_MergeAppend:
			children = castNode(MergeAppend, plan)->mergeplans;
			break;
		case T_BitmapAnd:
			children = castNode(BitmapAnd, plan)->bitmapplans;
			break;
		case T_BitmapOr:
			children = castNode(BitmapOr, plan)->bitmapplans;
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		case T_SubqueryScan:
			bloompg_access_plan_shape(castNode(SubqueryScan, plan)->subplan,
				shape);
			break;
		default:
			break;
	}
	foreach(cell, children)
		bloompg_access_plan_shape((Plan *) lfirst(cell), shape);
}

static List *
bloompg_access_filter_metadata(List *filters)
{
	List *metadata = NIL;
	ListCell *cell;

	foreach(cell, filters)
	{
		BloomPGSampleFilter *filter = lfirst(cell);
		List *attributes = NIL;
		int key;

		if (filter->materialized_attributes == NULL)
			return NIL;
		for (key = 0; key < filter->nkeys; key++)
		{
			if (!AttributeNumberIsValid(
					filter->materialized_attributes[key]))
				return NIL;
			attributes = lappend_int(attributes,
				filter->materialized_attributes[key]);
		}
		metadata = lappend(metadata, list_make3(
			makeString(psprintf(UINT64_FORMAT, filter->token)),
			makeInteger(filter->nkeys), attributes));
	}
	return metadata;
}

bool
bloompg_plan_transfer_access(BloomPGRelationState *state,
	const char *query_string, ParamListInfo bound_params,
	PlannedStmt **result)
{
	SeqScan *semantic_scan;
	Query *query;
	RangeTblRef *reference;
	RangeTblEntry *rte;
	List *quals;
	List *metadata;
	ListCell *cell;
	BloomPGAccessExactCandidate *exact_candidates;
	int exact_candidate_count = 0;
	int exact_candidate_index;
	PlannedStmt *statement;
	BloomPGAccessPlanShape shape = {0};
	instr_time started;
	uint64 exact_filters = 0;
	uint64 exact_keys = 0;

	*result = NULL;
	state->access_plan_attempts++;
	state->access_plan_rejection = NULL;
	if (!bloompg_index_transfer || state->analysis_query == NULL ||
		state->table == NULL || state->table->rte_kind != RTE_RELATION ||
		!OidIsValid(state->table->relation_oid) || state->filters == NIL ||
		state->rti == 0 ||
		state->rti > (Index) list_length(state->analysis_query->rtable))
	{
		state->access_plan_rejection = "ineligible-relation";
		return false;
	}
	rte = list_nth_node(RangeTblEntry, state->analysis_query->rtable,
		state->rti - 1);
	if (rte->rtekind != RTE_RELATION ||
		rte->relid != state->table->relation_oid)
	{
		state->access_plan_rejection = "rte-mismatch";
		return false;
	}
	semantic_scan = bloompg_make_seqscan_evaluator(state->fallback_plan);
	if (semantic_scan == NULL ||
		!bloompg_access_expressions_safe(
			(Node *) semantic_scan->scan.plan.targetlist, state->rti) ||
		!bloompg_access_expressions_safe(
			(Node *) semantic_scan->scan.plan.qual, state->rti))
	{
		state->access_plan_rejection = "unsafe-base-expression";
		return false;
	}
	quals = copyObject(semantic_scan->scan.plan.qual);
	exact_candidates = palloc0_array(BloomPGAccessExactCandidate,
		list_length(state->filters));
	foreach(cell, state->filters)
	{
		BloomPGSampleFilter *filter = lfirst(cell);
		BloomPGFilter *runtime_filter = filter->filter;
		TypeCacheEntry *type_cache;
		uint64 keys;
		double fraction;

		if (runtime_filter == NULL || !runtime_filter->valid ||
			runtime_filter->kind != BLOOMPG_FILTER_BITMAP ||
			runtime_filter->nkeys != 1 || filter->nkeys != 1 ||
			filter->attributes == NULL ||
			!AttributeNumberIsValid(filter->attributes[0]) ||
			runtime_filter->keys[0].build_type !=
				runtime_filter->keys[0].probe_type)
			continue;
		keys = bloompg_bitmap_distinct_keys(runtime_filter);
		if (keys == 0 ||
			keys > (uint64) bloompg_index_transfer_planner_max_keys)
			continue;
		type_cache = lookup_type_cache(runtime_filter->keys[0].probe_type,
			TYPECACHE_EQ_OPR);
		if (type_cache == NULL || !OidIsValid(type_cache->eq_opr) ||
			!bloompg_relation_has_transfer_index(state, filter->attributes[0],
				type_cache->eq_opr, runtime_filter->keys[0].collation))
			continue;
		if (!bloompg_sample_workspace_filter_fraction(state->sample_workspace,
				filter, &fraction, NULL, NULL, NULL))
			fraction = state->filter_sample_fraction;
		exact_candidates[exact_candidate_count].sample_filter = filter;
		exact_candidates[exact_candidate_count].keys = keys;
		exact_candidates[exact_candidate_count].fraction =
			Min(Max(fraction, 0.0), 1.0);
		exact_candidate_count++;
	}
	qsort(exact_candidates, exact_candidate_count,
		sizeof(BloomPGAccessExactCandidate),
		bloompg_compare_exact_candidates);
	for (exact_candidate_index = 0;
		exact_candidate_index < exact_candidate_count;
		exact_candidate_index++)
	{
		BloomPGAccessExactCandidate *candidate =
			&exact_candidates[exact_candidate_index];
		ScalarArrayOpExpr *qual;
		uint64 keys;

		if (candidate->keys >
			(uint64) bloompg_index_transfer_planner_max_keys - exact_keys)
			continue;
		qual = bloompg_make_exact_array_qual(state,
			candidate->sample_filter, &keys);
		if (qual == NULL || keys != candidate->keys)
			continue;
		quals = lappend(quals, qual);
		exact_filters++;
		exact_keys += keys;
	}
	state->access_plan_exact_filters += exact_filters;
	state->access_plan_exact_keys += exact_keys;
	if (exact_filters == 0)
	{
		state->access_plan_rejection = "no-enumerable-exact-filter";
		return false;
	}
	metadata = bloompg_access_filter_metadata(state->filters);
	if (metadata == NIL)
	{
		state->access_plan_rejection = "invalid-output-filter-map";
		return false;
	}

	query = makeNode(Query);
	query->commandType = CMD_SELECT;
	query->querySource = QSRC_ORIGINAL;
	query->canSetTag = false;
	query->rtable = copyObject(state->analysis_query->rtable);
	query->rteperminfos = copyObject(state->analysis_query->rteperminfos);
	/* PostgreSQL 18 records the presence of an RTE_GROUP separately from the
	 * range table.  The private access query preserves RT indexes by copying
	 * that table, so its consistency flag must travel with it even though the
	 * selected base-scan target list contains no grouping Vars. */
	query->hasGroupRTE = state->analysis_query->hasGroupRTE;
	query->targetList = copyObject(semantic_scan->scan.plan.targetlist);
	reference = makeNode(RangeTblRef);
	reference->rtindex = state->rti;
	query->jointree = makeFromExpr(list_make1(reference),
		(Node *) make_ands_explicit(quals));
	query->stmt_location = state->analysis_query->stmt_location;
	query->stmt_len = state->analysis_query->stmt_len;

	INSTR_TIME_SET_CURRENT(started);
	statement = bloompg_plan_auxiliary_query(query,
		query_string != NULL ? query_string : "<bloompg transfer access>",
		state->cursor_options & CURSOR_OPT_PARALLEL_OK, bound_params);
	state->access_plan_ms += bloompg_transfer_elapsed_ms(started);
	if (statement == NULL || statement->planTree == NULL ||
		!bms_is_empty(statement->planTree->extParam))
	{
		state->access_plan_rejection = "planner-returned-parameterized-plan";
		return false;
	}
	bloompg_access_plan_shape(statement->planTree, &shape);
	state->access_plan_startup_cost = statement->planTree->startup_cost;
	state->access_plan_total_cost = statement->planTree->total_cost;
	state->access_plan_rows = statement->planTree->plan_rows;
	state->access_plan_seq_scans += shape.seq_scans;
	state->access_plan_index_scans += shape.index_scans;
	state->access_plan_index_only_scans += shape.index_only_scans;
	state->access_plan_bitmap_heap_scans += shape.bitmap_heap_scans;
	state->access_plan_bitmap_index_scans += shape.bitmap_index_scans;
	state->access_plan_gathers += shape.gathers;
	state->access_plan_parallel_safe = statement->planTree->parallel_safe;
	if (shape.bitmap_heap_scans > 0 || shape.bitmap_index_scans > 0)
		state->access_plan_kind = "core-bitmap";
	else if (shape.index_only_scans > 0)
		state->access_plan_kind = "core-index-only";
	else if (shape.index_scans > 0)
		state->access_plan_kind = "core-index";
	else if (shape.gathers > 0 && shape.seq_scans > 0)
		state->access_plan_kind = "core-parallel-seq";
	else if (shape.seq_scans > 0)
		state->access_plan_kind = "core-seq";
	else
		state->access_plan_kind = "core-other";

	/* The existing vector/parallel path is a better residual-filter engine if
	 * core found no index access at all.  Retain it as the physical candidate
	 * in that case; the auxiliary plan and its exact clauses are disposable. */
	if (shape.index_scans == 0 && shape.index_only_scans == 0 &&
		shape.bitmap_heap_scans == 0 && shape.bitmap_index_scans == 0)
	{
		state->access_plan_rejection = "core-selected-no-index";
		return false;
	}
	statement->planTree = bloompg_make_filter_scan(statement->planTree,
		metadata, bloompg_max_plan_node_id(statement->planTree) + 1,
		false, false);
	statement->canSetTag = false;
	statement->jitFlags = 0;
	state->materialization_access_path = state->access_plan_kind;
	*result = statement;
	return true;
}
