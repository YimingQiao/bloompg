#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/filter.h"
#include "bloompg/sampling.h"
#include "bloompg/scan_quals.h"
#include "bloompg/transfer_internal.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/paths.h"

#include <math.h>
#include "executor/instrument.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/typcache.h"

/* Exact integral key sets drive PostgreSQL's native B-tree scan interfaces.
 * Each set is passed once as a complete array scan key, without SQL planning
 * or manual batches. Heap tuples always recheck every filter and local qual,
 * including when work_mem forces the TID bitmap to use lossy pages. */

typedef struct BloomPGIndexCandidate
{
	BloomPGSampleFilter *sample_filter;
	Oid			index_oid;
	Oid			equality_operator;
	uint64		distinct_keys;
	double		estimated_fraction;
	uint64		sample_rows;
	uint64		sample_matches;
	double		estimate_ms;
	const char *estimate_source;
	uint64		candidates;
} BloomPGIndexCandidate;

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
bloompg_index_matches_filter(Relation index_relation,
	BloomPGSampleFilter *sample_filter, Oid equality_operator)
{
	BloomPGFilter *filter = sample_filter->filter;

	if (index_relation->rd_rel->relam != BTREE_AM_OID ||
		index_relation->rd_index == NULL ||
		!index_relation->rd_index->indisvalid ||
		!index_relation->rd_index->indisready ||
		!index_relation->rd_index->indislive ||
		index_relation->rd_index->indnkeyatts < 1 ||
		index_relation->rd_index->indkey.values[0] <= 0 ||
		index_relation->rd_index->indkey.values[0] !=
			sample_filter->attributes[0] ||
		RelationGetIndexPredicate(index_relation) != NIL ||
		get_op_opfamily_strategy(equality_operator,
			index_relation->rd_opfamily[0]) != BTEqualStrategyNumber)
		return false;
	if (OidIsValid(filter->keys[0].collation) &&
		index_relation->rd_indcollation[0] != filter->keys[0].collation)
		return false;
	return true;
}

static bool
bloompg_index_candidate_less(const BloomPGIndexCandidate *left,
	const BloomPGIndexCandidate *right)
{
	if (left->distinct_keys != right->distinct_keys)
		return left->distinct_keys < right->distinct_keys;
	if (left->sample_filter->token != right->sample_filter->token)
		return left->sample_filter->token < right->sample_filter->token;
	return left->index_oid < right->index_oid;
}

static int
bloompg_compare_index_candidates(const void *left, const void *right)
{
	const BloomPGIndexCandidate *a = left;
	const BloomPGIndexCandidate *b = right;

	if (bloompg_index_candidate_less(a, b))
		return -1;
	if (bloompg_index_candidate_less(b, a))
		return 1;
	return 0;
}

static int
bloompg_collect_index_candidates(Relation heap_relation,
	BloomPGRelationState *state, bool require_selective,
	BloomPGIndexCandidate **result)
{
	List	   *index_oids;
	ListCell   *filter_cell;
	BloomPGIndexCandidate *found;
	int		found_count = 0;
	uint64		candidates = 0;

	*result = NULL;
	if (!bloompg_index_transfer || state->filters == NIL)
		return 0;
	found = palloc0_array(BloomPGIndexCandidate,
		list_length(state->filters));
	index_oids = RelationGetIndexList(heap_relation);
	foreach(filter_cell, state->filters)
	{
		BloomPGSampleFilter *sample_filter = lfirst(filter_cell);
		BloomPGFilter *filter = sample_filter->filter;
		TypeCacheEntry *type_cache;
		uint64		distinct_keys;
		double		estimated_fraction = -1.0;
		uint64		sample_rows = 0;
		uint64		sample_matches = 0;
		double		estimate_ms = 0.0;
		const char *estimate_source = "sample";
		ListCell   *index_cell;
		Oid			best_index_oid = InvalidOid;
		BlockNumber best_index_blocks = InvalidBlockNumber;

		if (filter == NULL || !filter->valid ||
			filter->kind != BLOOMPG_FILTER_BITMAP || filter->nkeys != 1 ||
			sample_filter->nkeys != 1 || sample_filter->attributes == NULL ||
			!AttributeNumberIsValid(sample_filter->attributes[0]) ||
			filter->keys[0].build_type != filter->keys[0].probe_type)
			continue;
		type_cache = lookup_type_cache(filter->keys[0].probe_type,
			TYPECACHE_EQ_OPR);
		if (type_cache == NULL || !OidIsValid(type_cache->eq_opr))
			continue;
		distinct_keys = bloompg_bitmap_distinct_keys(filter);
		if (distinct_keys == 0)
			continue;
		foreach(index_cell, index_oids)
		{
			Oid index_oid = lfirst_oid(index_cell);
			Relation index_relation = index_open(index_oid, AccessShareLock);
			bool matches = bloompg_index_matches_filter(index_relation,
				sample_filter, type_cache->eq_opr);
			BlockNumber index_blocks = matches ?
				RelationGetNumberOfBlocks(index_relation) : InvalidBlockNumber;

			index_close(index_relation, AccessShareLock);
			if (!matches)
				continue;
			candidates++;
			if (!OidIsValid(best_index_oid) ||
				index_blocks < best_index_blocks)
			{
				best_index_oid = index_oid;
				best_index_blocks = index_blocks;
			}
		}
		if (!OidIsValid(best_index_oid))
			continue;
		/* Estimate each index condition independently of residual predicates.
		 * Rejecting an index source never removes its filtering condition. */
		if (bloompg_sample_workspace_filter_fraction(state->sample_workspace,
				sample_filter, &estimated_fraction, &sample_rows,
				&sample_matches, &estimate_ms))
			estimated_fraction = Min(Max(estimated_fraction, 0.0), 1.0);
		else
		{
			estimated_fraction = -1.0;
			estimate_source = "unavailable";
		}
		if (estimated_fraction < 0.0 ||
			estimated_fraction > bloompg_index_transfer_max_fraction ||
			(require_selective &&
			 estimated_fraction > bloompg_index_priority_max_fraction))
			continue;
		found[found_count].sample_filter = sample_filter;
		found[found_count].index_oid = best_index_oid;
		found[found_count].equality_operator = type_cache->eq_opr;
		found[found_count].distinct_keys = distinct_keys;
		found[found_count].estimated_fraction = estimated_fraction;
		found[found_count].sample_rows = sample_rows;
		found[found_count].sample_matches = sample_matches;
		found[found_count].estimate_ms = estimate_ms;
		found[found_count].estimate_source = estimate_source;
		found_count++;
	}
	list_free(index_oids);
	if (found_count == 0)
	{
		pfree(found);
		return 0;
	}
	qsort(found, found_count, sizeof(BloomPGIndexCandidate),
		bloompg_compare_index_candidates);
	for (int index = 0; index < found_count; index++)
		found[index].candidates = candidates;
	*result = found;
	return found_count;
}

static bool
bloompg_find_index_candidate(Relation heap_relation,
	BloomPGRelationState *state, bool require_selective, BloomPGIndexCandidate *result,
	int *dominated_count);

static bool
bloompg_bitmap_filter_subset(const BloomPGFilter *subset,
	const BloomPGFilter *superset)
{
	uint64		word_count;
	uint64		word_index;

	if (subset == NULL || superset == NULL || !subset->valid ||
		!superset->valid || subset->kind != BLOOMPG_FILTER_BITMAP ||
		superset->kind != BLOOMPG_FILTER_BITMAP || subset->nkeys != 1 ||
		superset->nkeys != 1 ||
		subset->keys[0].build_type != superset->keys[0].build_type ||
		subset->keys[0].probe_type != superset->keys[0].probe_type)
		return false;
	word_count = subset->nbits / 64;
	for (word_index = 0; word_index < word_count; word_index++)
	{
		uint64 word = subset->words[word_index];

		if ((word_index & UINT64CONST(1023)) == 0)
			CHECK_FOR_INTERRUPTS();
		while (word != 0)
		{
			int bit = pg_rightmost_one_pos64(word);
			uint64 bit_offset = word_index * 64 + (uint64) bit;
			int64 value = subset->bitmap_lower + (int64) bit_offset;
			uint64 superset_offset;

			word &= word - 1;
			if (value > subset->bitmap_upper)
				continue;
			if (value < superset->bitmap_lower ||
				value > superset->bitmap_upper)
				return false;
			superset_offset = (uint64) value -
				(uint64) superset->bitmap_lower;
			if (superset_offset >= superset->nbits ||
				(superset->words[superset_offset >> 6] &
				 (UINT64CONST(1) << (superset_offset & 63))) == 0)
				return false;
		}
	}
	return true;
}

static bool
bloompg_index_candidates_share_domain(const BloomPGIndexCandidate *left,
	const BloomPGIndexCandidate *right)
{
	const BloomPGFilter *left_filter = left->sample_filter->filter;
	const BloomPGFilter *right_filter = right->sample_filter->filter;

	return left->index_oid == right->index_oid &&
		left->sample_filter->attributes[0] ==
			right->sample_filter->attributes[0] &&
		left->equality_operator == right->equality_operator &&
		left_filter->keys[0].build_type ==
			right_filter->keys[0].build_type &&
		left_filter->keys[0].probe_type ==
			right_filter->keys[0].probe_type &&
		left_filter->keys[0].collation ==
			right_filter->keys[0].collation;
}

/*
 * Multiple formal filters can constrain the same indexed column.  If one
 * exact value set is contained by another, probing the wider set cannot add a
 * surviving heap tuple: the narrower formal filter will reject every extra
 * tuple.  Remove only the redundant access source; all formal filters remain
 * attached to the materialization receiver and are still rechecked.
 */
static int
bloompg_mark_dominated_index_candidates(BloomPGIndexCandidate *candidates,
	int count, bool *dominated)
{
	int dominated_count = 0;
	int left;

	for (left = 0; left < count; left++)
	{
		int right;

		if (dominated[left])
			continue;
		for (right = left + 1; right < count; right++)
		{
			if (dominated[right] ||
				!bloompg_index_candidates_share_domain(&candidates[left],
					&candidates[right]))
				continue;
			if (candidates[left].distinct_keys <=
					candidates[right].distinct_keys &&
				bloompg_bitmap_filter_subset(
					candidates[left].sample_filter->filter,
					candidates[right].sample_filter->filter))
			{
				dominated[right] = true;
				dominated_count++;
			}
			else if (candidates[right].distinct_keys <
					candidates[left].distinct_keys &&
				bloompg_bitmap_filter_subset(
					candidates[right].sample_filter->filter,
					candidates[left].sample_filter->filter))
			{
				dominated[left] = true;
				dominated_count++;
				break;
			}
		}
	}
	return dominated_count;
}

static bool
bloompg_find_index_candidate(Relation heap_relation,
	BloomPGRelationState *state, bool require_selective, BloomPGIndexCandidate *result,
	int *dominated_count)
{
	BloomPGIndexCandidate *candidates;
	bool *dominated;
	int count;
	int selected = -1;
	int index;

	MemSet(result, 0, sizeof(*result));
	if (dominated_count != NULL)
		*dominated_count = 0;
	count = bloompg_collect_index_candidates(heap_relation, state,
		require_selective, &candidates);
	if (count == 0)
		return false;
	dominated = palloc0_array(bool, count);
	if (dominated_count != NULL)
		*dominated_count = bloompg_mark_dominated_index_candidates(candidates,
			count, dominated);
	else
		(void) bloompg_mark_dominated_index_candidates(candidates, count,
			dominated);
	for (index = 0; index < count; index++)
	{
		if (!dominated[index])
		{
			selected = index;
			break;
		}
	}
	Assert(selected >= 0);
	*result = candidates[selected];
	pfree(dominated);
	pfree(candidates);
	return true;
}

static bool
bloompg_integral_scan_datum(int64 value, Oid type_oid, Datum *result)
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

/* The same exact key array drives serial index_getbitmap and the native
 * parallel BitmapIndexScan. No SQL text or auxiliary planner is involved. */
static Datum *
bloompg_index_key_values(BloomPGIndexCandidate *candidate, int *value_count)
{
	BloomPGFilter *filter = candidate->sample_filter->filter;
	Datum *values;

	if (candidate->distinct_keys > INT_MAX ||
		candidate->distinct_keys > MaxAllocSize / sizeof(Datum))
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			errmsg("transfer key array exceeds PostgreSQL allocation limits")));
	values = palloc_array(Datum, candidate->distinct_keys);
	*value_count = 0;
	for (uint64 word_index = 0; word_index < filter->nbits / 64; word_index++)
	{
		uint64 word = filter->words[word_index];

		CHECK_FOR_INTERRUPTS();
		while (word != 0)
		{
			int bit = pg_rightmost_one_pos64(word);
			int64 value = filter->bitmap_lower +
				(int64) (word_index * 64 + (uint64) bit);
			Datum argument;

			word &= word - 1;
			if (value <= filter->bitmap_upper &&
				bloompg_integral_scan_datum(value, filter->keys[0].build_type,
					&argument))
				values[(*value_count)++] = argument;
		}
	}
	Assert((uint64) *value_count == candidate->distinct_keys);
	return values;
}

static void
bloompg_bitmap_array_scan(IndexScanDesc index_scan, ScanKey scan_key,
	TIDBitmap *bitmap, Datum *values, int value_count, Oid element_type,
	uint64 *index_tuples)
{
	ArrayType  *array;
	int16		type_length;
	bool		type_by_value;
	char		type_alignment;
	int64		tuples;

	Assert(value_count > 0);
	get_typlenbyvalalign(element_type, &type_length, &type_by_value,
		&type_alignment);
	array = construct_array(values, value_count, element_type, type_length,
		type_by_value, type_alignment);
	scan_key->sk_argument = PointerGetDatum(array);
	index_rescan(index_scan, scan_key, 1, NULL, 0);
	tuples = index_getbitmap(index_scan, bitmap);
	if (tuples > 0)
		*index_tuples += (uint64) tuples;
	pfree(array);
}

static int
bloompg_select_bitmap_candidates(BloomPGIndexCandidate *candidates,
	int count, int *selected, int *dominated_count)
{
	bool *dominated = palloc0_array(bool, count);
	int selected_count = 0;
	int index;

	*dominated_count = bloompg_mark_dominated_index_candidates(candidates,
		count, dominated);
	for (index = 0; index < count; index++)
		if (!dominated[index])
			selected[selected_count++] = index;
	pfree(dominated);
	return selected_count;
}

static void
bloompg_record_index_candidate(BloomPGRelationState *state,
	BloomPGIndexCandidate *candidate, int dominated_count)
{
	state->index_probe_fraction = candidate->estimated_fraction;
	state->index_probe_oid = candidate->index_oid;
	state->index_probe_attribute = candidate->sample_filter->attributes[0];
	state->index_probe_filter_token = candidate->sample_filter->token;
	state->index_probe_candidates += candidate->candidates;
	state->index_probe_sample_rows += candidate->sample_rows;
	state->index_probe_sample_matches += candidate->sample_matches;
	state->index_probe_estimate_ms += candidate->estimate_ms;
	state->index_probe_estimate_source = candidate->estimate_source;
	state->index_probe_dominated_candidates += dominated_count;
}

/* PostgreSQL owns shared scan state, work distribution, rechecks, worker
 * startup, and cancellation for native bitmap heap scans. */
Plan *
bloompg_make_parallel_index_source(BloomPGRelationState *state,
	int *next_plan_node_id, ParamListInfo bound_params, ParamListInfo *scan_params)
{
	Relation relation = table_open(state->table->relation_oid, AccessShareLock);
	BloomPGIndexCandidate *candidates;
	int count = bloompg_collect_index_candidates(relation, state, false,
		&candidates);
	int *selected;
	int selected_count;
	int dominated_count;
	double estimated_rows = state->base_rows;
	double heap_pages = (double) RelationGetNumberOfBlocks(relation);
	double estimated_pages;
	SeqScan *base;
	BitmapHeapScan *heap;
	List *index_plans = NIL;
	List *rechecks = NIL;
	int existing_params = bound_params != NULL ? bound_params->numParams : 0;
	ParamListInfo params;

	table_close(relation, AccessShareLock);
	if (count == 0)
		return NULL;
	selected = palloc_array(int, count);
	selected_count = bloompg_select_bitmap_candidates(candidates, count,
		selected, &dominated_count);
	for (int i = 0; i < selected_count; i++)
		estimated_rows = Min(estimated_rows, state->base_rows *
			candidates[selected[i]].estimated_fraction);
	/* Approximate distinct pages for scattered hits. Reuse PostgreSQL's
	 * minimum parallel table-scan size; a small result spread over many
	 * heap pages can still benefit from parallel retrieval. */
	estimated_pages = heap_pages > 0 ?
		-heap_pages * expm1(-estimated_rows / heap_pages) : 0;
	if (estimated_pages < (double) Max(min_parallel_table_scan_size, 1))
	{
		pfree(selected);
		pfree(candidates);
		return NULL;
	}
	/* Parameters serialize each array once in binary form. Embedding Const
	 * arrays in indexqual, indexqualorig and bitmapqualorig would duplicate
	 * large key sets throughout the plan sent to every worker. */
	params = makeParamList(existing_params + selected_count);
	if (existing_params > 0)
	{
		ParamListInfo original = copyParamList(bound_params);

		memcpy(params->params, original->params,
			existing_params * sizeof(ParamExternData));
	}
	base = bloompg_make_seqscan_evaluator(state->fallback_plan);
	if (base == NULL)
		elog(ERROR, "bloompg cannot normalize the parallel bitmap scan");
	heap = makeNode(BitmapHeapScan);
	heap->scan = base->scan;
	NodeSetTag(heap, T_BitmapHeapScan);
	heap->scan.plan.plan_node_id = (*next_plan_node_id)++;
	heap->scan.plan.parallel_aware = true;
	heap->scan.plan.parallel_safe = true;
	for (int i = 0; i < selected_count; i++)
	{
		BloomPGIndexCandidate *candidate = &candidates[selected[i]];
		BloomPGFilter *filter = candidate->sample_filter->filter;
		Oid type = filter->keys[0].build_type;
		int value_count;
		Datum *values = bloompg_index_key_values(candidate, &value_count);
		int16 length;
		bool byval;
		char align;
		ArrayType *array;
		Param *keys = makeNode(Param);
		ScalarArrayOpExpr *qual = makeNode(ScalarArrayOpExpr);
		ScalarArrayOpExpr *indexqual;
		BitmapIndexScan *index = makeNode(BitmapIndexScan);
		Var *var = makeVar(state->rti, candidate->sample_filter->attributes[0],
			type, -1, filter->keys[0].collation, 0);

		get_typlenbyvalalign(type, &length, &byval, &align);
		array = construct_array(values, value_count, type, length, byval, align);
		pfree(values);
		qual->opno = candidate->equality_operator;
		qual->opfuncid = get_opcode(qual->opno);
		/* Lossy heap pages must recheck array membership. Use PostgreSQL's
		 * hashed SAOP evaluation rather than a linear scan of a large array. */
		{
			Oid left_hash;
			Oid right_hash;

			if (get_op_hash_functions(qual->opno, &left_hash, &right_hash) &&
				left_hash == right_hash)
				qual->hashfuncid = left_hash;
		}
		qual->useOr = true;
		qual->inputcollid = filter->keys[0].collation;
		qual->location = -1;
		keys->paramkind = PARAM_EXTERN;
		keys->paramid = existing_params + i + 1;
		keys->paramtype = get_array_type(type);
		keys->paramtypmod = -1;
		keys->paramcollid = InvalidOid;
		keys->location = -1;
		params->params[keys->paramid - 1].value = PointerGetDatum(array);
		params->params[keys->paramid - 1].isnull = false;
		params->params[keys->paramid - 1].pflags = PARAM_FLAG_CONST;
		params->params[keys->paramid - 1].ptype = keys->paramtype;
		qual->args = list_make2(var, keys);
		indexqual = copyObject(qual);
		var = linitial_node(Var, indexqual->args);
		var->varno = INDEX_VAR;
		var->varattno = 1;
		index->scan.scanrelid = state->rti;
		index->scan.plan.plan_node_id = (*next_plan_node_id)++;
		index->scan.plan.parallel_safe = true;
		index->indexid = candidate->index_oid;
		index->isshared = true;
		index->indexqual = list_make1(indexqual);
		index->indexqualorig = list_make1(qual);
		index_plans = lappend(index_plans, index);
		rechecks = lappend(rechecks, qual);
		if (i < BLOOMPG_MAX_INDEX_BITMAP_SOURCES)
		{
			BloomPGIndexBitmapInputProfile *input = &state->index_bitmap_inputs[i];

			input->plan_node_id = index->scan.plan.plan_node_id;
			input->index_oid = candidate->index_oid;
			input->attribute = candidate->sample_filter->attributes[0];
			input->filter_token = candidate->sample_filter->token;
			input->keys = candidate->distinct_keys;
			input->estimated_fraction = candidate->estimated_fraction;
			input->index_tuples = 0;
		}

	}
	if (list_length(index_plans) == 1)
		heap->scan.plan.lefttree = linitial(index_plans);
	else
	{
		BitmapAnd *intersection = makeNode(BitmapAnd);

		intersection->plan.plan_node_id = (*next_plan_node_id)++;
		intersection->plan.parallel_safe = true;
		intersection->bitmapplans = index_plans;
		heap->scan.plan.lefttree = (Plan *) intersection;
	}
	heap->bitmapqualorig = rechecks;
	*scan_params = params;
	state->index_bitmap_input_count =
		Min(selected_count, BLOOMPG_MAX_INDEX_BITMAP_SOURCES);
	bloompg_record_index_candidate(state, &candidates[selected[0]], dominated_count);
	pfree(selected);
	pfree(candidates);
	return (Plan *) heap;
}

static TIDBitmap *
bloompg_build_candidate_bitmap(Snapshot snapshot,
	BloomPGIndexCandidate *candidate, uint64 *keys_probed,
	uint64 *batches, uint64 *index_tuples)
{
	BloomPGFilter *filter = candidate->sample_filter->filter;
	Relation index_relation = NULL;
	IndexScanDesc index_scan = NULL;
	TIDBitmap *bitmap = NULL;
	ScanKeyData scan_key;
	Datum *values = NULL;
	int value_count = 0;
	Size bitmap_bytes = mul_size((Size) Max(work_mem, 1), (Size) 1024);

	index_relation = index_open(candidate->index_oid, AccessShareLock);
	if (!bloompg_index_matches_filter(index_relation,
			candidate->sample_filter, candidate->equality_operator))
	{
		index_close(index_relation, AccessShareLock);
		return NULL;
	}
	bitmap = tbm_create(bitmap_bytes, NULL);
	ScanKeyEntryInitialize(&scan_key, SK_SEARCHARRAY, 1,
		BTEqualStrategyNumber, filter->keys[0].build_type,
		index_relation->rd_indcollation[0],
		get_opcode(candidate->equality_operator), (Datum) 0);
	values = bloompg_index_key_values(candidate, &value_count);
	PG_TRY();
	{
		index_scan = index_beginscan_bitmap(index_relation, snapshot, NULL, 1);
		bloompg_bitmap_array_scan(index_scan, &scan_key, bitmap,
			values, value_count, filter->keys[0].build_type, index_tuples);
		*keys_probed += (uint64) value_count;
		(*batches)++;
	}
	PG_FINALLY();
	{
		if (index_scan != NULL)
			index_endscan(index_scan);
		if (values != NULL)
			pfree(values);
		index_close(index_relation, AccessShareLock);
	}
	PG_END_TRY();
	return bitmap;
}

static bool
bloompg_bitmap_prefilter_scan(BloomPGRelationState *state,
	Relation heap_relation, Snapshot snapshot,
	BloomPGIndexTupleCallback callback, void *callback_context)
{
	BloomPGIndexCandidate *candidates;
	int *selected;
	int count;
	int selected_count;
	int dominated_count;
	TIDBitmap *combined = NULL;
	TableScanDesc table_scan = NULL;
	TupleTableSlot *slot = NULL;
	instr_time started;
	uint64 keys_probed = 0;
	uint64 batches = 0;
	uint64 index_tuples = 0;
	uint64 rows_fetched = 0;
	uint64 exact_pages = 0;
	uint64 lossy_pages = 0;
	bool keep_scanning = true;
	int built_sources = 0;
	BloomPGIndexCandidate *primary_candidate = NULL;
	int selected_index;

	count = bloompg_collect_index_candidates(heap_relation, state,
		false, &candidates);
	if (count == 0)
		return false;
	selected = palloc_array(int, count);
	selected_count = bloompg_select_bitmap_candidates(candidates, count,
		selected, &dominated_count);
	INSTR_TIME_SET_CURRENT(started);
	PG_TRY();
	{
		for (selected_index = 0; selected_index < selected_count;
			selected_index++)
		{
			BloomPGIndexCandidate *candidate =
				&candidates[selected[selected_index]];
			uint64 source_keys = 0;
			uint64 source_batches = 0;
			uint64 source_index_tuples = 0;
			TIDBitmap *next = bloompg_build_candidate_bitmap(snapshot,
				candidate, &source_keys, &source_batches,
				&source_index_tuples);

			if (next == NULL)
				continue;
			keys_probed += source_keys;
			batches += source_batches;
			index_tuples += source_index_tuples;
			if (primary_candidate == NULL)
				primary_candidate = candidate;
			if (built_sources < BLOOMPG_MAX_INDEX_BITMAP_SOURCES)
			{
				BloomPGIndexBitmapInputProfile *input =
					&state->index_bitmap_inputs[built_sources];

				input->index_oid = candidate->index_oid;
				input->attribute =
					candidate->sample_filter->attributes[0];
				input->filter_token = candidate->sample_filter->token;
				input->keys = source_keys;
				input->estimated_fraction = candidate->estimated_fraction;
				input->index_tuples = source_index_tuples;
			}
			built_sources++;
			if (combined == NULL)
				combined = next;
			else
			{
				tbm_intersect(combined, next);
				tbm_free(next);
			}
			if (tbm_is_empty(combined))
				break;
		}
		if (combined != NULL && !tbm_is_empty(combined))
		{
			bool recheck;

			table_scan = table_beginscan_bm(heap_relation, snapshot, 0, NULL);
			table_scan->st.rs_tbmiterator = tbm_begin_iterate(combined, NULL,
				InvalidDsaPointer);
			slot = table_slot_create(heap_relation, NULL);
			while (table_scan_bitmap_next_tuple(table_scan, slot, &recheck,
					&lossy_pages, &exact_pages))
			{
				(void) recheck;
				CHECK_FOR_INTERRUPTS();
				rows_fetched++;
				if (!callback(slot, callback_context))
				{
					keep_scanning = false;
					break;
				}
			}
		}
	}
	PG_FINALLY();
	{
		if (slot != NULL)
			ExecDropSingleTupleTableSlot(slot);
		if (table_scan != NULL)
		{
			if (!tbm_exhausted(&table_scan->st.rs_tbmiterator))
				tbm_end_iterate(&table_scan->st.rs_tbmiterator);
			table_endscan(table_scan);
		}
		if (combined != NULL)
			tbm_free(combined);
	}
	PG_END_TRY();
	(void) keep_scanning;
	if (built_sources == 0 || primary_candidate == NULL)
	{
		pfree(selected);
		pfree(candidates);
		return false;
	}
	state->index_probe_scans++;
	state->index_probe_batches += batches;
	state->index_probe_keys += keys_probed;
	state->index_probe_rows += rows_fetched;
	state->index_probe_ms += bloompg_transfer_elapsed_ms(started);
	bloompg_record_index_candidate(state, primary_candidate, dominated_count);
	state->index_bitmap_scans++;
	state->index_bitmap_sources += (uint64) built_sources;
	state->index_bitmap_index_tuples += index_tuples;
	state->index_bitmap_exact_pages += exact_pages;
	state->index_bitmap_lossy_pages += lossy_pages;
	state->index_bitmap_input_count = Min(built_sources,
		BLOOMPG_MAX_INDEX_BITMAP_SOURCES);
	state->materialization_access_path = "bitmap-index-probe";
	pfree(selected);
	pfree(candidates);
	return true;
}

bool
bloompg_index_prefilter_available(BloomPGRelationState *state,
	bool require_selective)
{
	BloomPGIndexCandidate candidate;
	Relation	heap_relation;
	bool		available;

	if (state == NULL || state->table == NULL ||
		state->table->rte_kind != RTE_RELATION ||
		!OidIsValid(state->table->relation_oid))
		return false;
	heap_relation = table_open(state->table->relation_oid, AccessShareLock);
	available = bloompg_find_index_candidate(heap_relation, state, require_selective,
		&candidate, NULL);
	table_close(heap_relation, AccessShareLock);
	return available;
}

bool
bloompg_relation_has_transfer_index(BloomPGRelationState *state,
	AttrNumber attribute, Oid equality_operator, Oid collation)
{
	Relation heap_relation;
	List *index_oids;
	ListCell *cell;
	Oid attribute_type;
	TypeCacheEntry *type_cache;
	bool found = false;

	if (!bloompg_index_transfer || state == NULL || state->table == NULL ||
		state->table->rte_kind != RTE_RELATION ||
		!OidIsValid(state->table->relation_oid) ||
		!AttributeNumberIsValid(attribute))
		return false;
	attribute_type = get_atttype(state->table->relation_oid, attribute);
	if (attribute_type != INT2OID && attribute_type != INT4OID &&
		attribute_type != INT8OID && attribute_type != OIDOID &&
		attribute_type != DATEOID)
		return false;
	type_cache = lookup_type_cache(attribute_type, TYPECACHE_EQ_OPR);
	if (type_cache == NULL || !OidIsValid(type_cache->eq_opr) ||
		(equality_operator != type_cache->eq_opr &&
		 get_commutator(equality_operator) != type_cache->eq_opr))
		return false;
	heap_relation = table_open(state->table->relation_oid, AccessShareLock);
	index_oids = RelationGetIndexList(heap_relation);
	foreach(cell, index_oids)
	{
		Oid index_oid = lfirst_oid(cell);
		Relation index_relation = index_open(index_oid, AccessShareLock);

		if (index_relation->rd_rel->relam == BTREE_AM_OID &&
			index_relation->rd_index != NULL &&
			index_relation->rd_index->indisvalid &&
			index_relation->rd_index->indisready &&
			index_relation->rd_index->indislive &&
			index_relation->rd_index->indnkeyatts > 0 &&
			index_relation->rd_index->indkey.values[0] == attribute &&
			get_op_opfamily_strategy(type_cache->eq_opr,
				index_relation->rd_opfamily[0]) == BTEqualStrategyNumber &&
			(!OidIsValid(collation) ||
			 index_relation->rd_indcollation[0] == collation))
			found = true;
		index_close(index_relation, AccessShareLock);
		if (found)
			break;
	}
	list_free(index_oids);
	table_close(heap_relation, AccessShareLock);
	return found;
}

bool
bloompg_index_prefilter_scan(BloomPGRelationState *state,
	Relation heap_relation, Snapshot snapshot,
	BloomPGIndexTupleCallback callback, void *callback_context)
{
	return bloompg_bitmap_prefilter_scan(state, heap_relation, snapshot,
		callback, callback_context);
}

/* Called after Gather has collected worker statistics, before ExecutorEnd. */
bool
bloompg_collect_parallel_index_profile(PlanState *node, void *context)
{
	BloomPGRelationState *state = context;

	if (node == NULL)
		return false;
	if (IsA(node, BitmapHeapScanState))
	{
		BitmapHeapScanState *heap = castNode(BitmapHeapScanState, node);

		state->index_probe_scans++;
		state->index_bitmap_scans++;
		state->index_bitmap_exact_pages += heap->stats.exact_pages;
		state->index_bitmap_lossy_pages += heap->stats.lossy_pages;
		if (heap->sinstrument != NULL)
		{
			for (int i = 0; i < heap->sinstrument->num_workers; i++)
			{
				state->index_bitmap_exact_pages += heap->sinstrument->sinstrument[i].exact_pages;
				state->index_bitmap_lossy_pages += heap->sinstrument->sinstrument[i].lossy_pages;
			}
		}
		if (node->instrument != NULL)
		{
			InstrEndLoop(node->instrument);
			state->index_probe_rows += (uint64) (node->instrument->ntuples +
				node->instrument->nfiltered1 + node->instrument->nfiltered2);
		}
	}
	else if (IsA(node, BitmapIndexScanState) && node->instrument != NULL)
	{
		BitmapIndexScan *index = castNode(BitmapIndexScan, node->plan);

		InstrEndLoop(node->instrument);
		if (node->instrument->nloops > 0)
		{
			ScalarArrayOpExpr *qual = linitial_node(ScalarArrayOpExpr, index->indexqual);
			Param *keys = lsecond_node(Param, qual->args);
			ArrayType *array = DatumGetArrayTypeP(
				node->state->es_param_list_info->params[keys->paramid - 1].value);

			state->index_probe_keys += ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));
			state->index_bitmap_sources++;
			state->index_probe_batches++;
			state->index_bitmap_index_tuples += (uint64) node->instrument->ntuples;
			for (int i = 0; i < state->index_bitmap_input_count; i++)
			{
				BloomPGIndexBitmapInputProfile *input = &state->index_bitmap_inputs[i];

				if (input->plan_node_id == index->scan.plan.plan_node_id)
				{
					input->index_tuples += (uint64) node->instrument->ntuples;
					break;
				}
			}
		}
	}
	return planstate_tree_walker(node, bloompg_collect_parallel_index_profile,
		context);
}
