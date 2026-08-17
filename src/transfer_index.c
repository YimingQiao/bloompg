#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/filter.h"
#include "bloompg/sampling.h"
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
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/typcache.h"

#include <math.h>

#define BLOOMPG_INDEX_PAGE_PROBE_KEYS UINT64CONST(256)

/*
 * Index-assisted transfer deliberately starts with the case for which Bloom
 * already owns a compact, exact and deduplicated key set: a one-column
 * canonical integral bitmap.  Iterating its set bits in value order gives a
 * batched, locality-friendly series of B-tree probes without constructing a
 * second hash table or a giant ScalarArrayOpExpr.  Every fetched tuple still
 * passes through the original scan qual and all incoming filters in
 * transfer_materialize.c, so the index is only an access-path optimization.
 */

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
	if (left->estimated_fraction != right->estimated_fraction)
		return left->estimated_fraction < right->estimated_fraction;
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
	BloomPGRelationState *state, BloomPGIndexCandidate **result)
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
		double		estimated_fraction;
		uint64		sample_rows = 0;
		uint64		sample_matches = 0;
		double		estimate_ms = 0.0;
		const char *estimate_source;
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
		if (distinct_keys == 0 ||
			distinct_keys > (uint64) bloompg_index_transfer_max_keys)
			continue;
		if (bloompg_sample_workspace_filter_fraction(state->sample_workspace,
				sample_filter, &estimated_fraction, &sample_rows,
				&sample_matches, &estimate_ms))
			estimate_source = "sample";
		else
		{
			estimated_fraction = state->filter_sample_fraction;
			estimate_source = "combined-fallback";
		}
		estimated_fraction = Min(Max(estimated_fraction, 0.0), 1.0);
		if (estimated_fraction > bloompg_index_transfer_max_fraction)
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
	BloomPGRelationState *state, BloomPGIndexCandidate *result,
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
	BloomPGRelationState *state, BloomPGIndexCandidate *result,
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
		&candidates);
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

static bool
bloompg_probe_array_batch(IndexScanDesc index_scan, ScanKey scan_key,
	TupleTableSlot *slot, Datum *values, int value_count, Oid element_type,
	BloomPGIndexTupleCallback callback, void *callback_context,
	uint64 *rows_fetched)
{
	ArrayType  *array;
	int16		type_length;
	bool		type_by_value;
	char		type_alignment;
	bool		keep_scanning = true;

	Assert(value_count > 0);
	get_typlenbyvalalign(element_type, &type_length, &type_by_value,
		&type_alignment);
	array = construct_array(values, value_count, element_type, type_length,
		type_by_value, type_alignment);
	scan_key->sk_argument = PointerGetDatum(array);
	index_rescan(index_scan, scan_key, 1, NULL, 0);
	while (index_getnext_slot(index_scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		(*rows_fetched)++;
		if (!callback(slot, callback_context))
		{
			keep_scanning = false;
			break;
		}
	}
	ExecClearTuple(slot);
	pfree(array);
	return keep_scanning;
}

static void
bloompg_bitmap_array_batch(IndexScanDesc index_scan, ScanKey scan_key,
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
bloompg_select_bitmap_candidates(BloomPGRelationState *state,
	BloomPGIndexCandidate *candidates, int count, int *selected,
	int *dominated_count)
{
	bool *dominated;
	double current_rows;
	int selected_count = 0;
	int candidate_index;

	*dominated_count = 0;
	if (count <= 0)
		return 0;
	dominated = palloc0_array(bool, count);
	*dominated_count = bloompg_mark_dominated_index_candidates(candidates,
		count, dominated);
	current_rows = 0.0;
	for (candidate_index = 0;
		candidate_index < count &&
			selected_count < BLOOMPG_MAX_INDEX_BITMAP_SOURCES;
		candidate_index++)
	{
		double build_rows;
		double saved_rows;

		if (dominated[candidate_index])
			continue;
		if (selected_count == 0)
		{
			selected[selected_count++] = candidate_index;
			current_rows = state->base_rows *
				candidates[candidate_index].estimated_fraction;
			continue;
		}
		build_rows = state->base_rows *
			candidates[candidate_index].estimated_fraction;
		saved_rows = current_rows *
			(1.0 - candidates[candidate_index].estimated_fraction);

		/* Building another bitmap is useful only if the extra index work is of
		 * the same order as the heap candidates it is expected to remove.  This
		 * deliberately avoids scanning a low-cardinality role index merely to
		 * shave a few rows from an already selective movie-id bitmap. */
		if (build_rows > saved_rows * 2.0)
			continue;
		selected[selected_count++] = candidate_index;
		current_rows *= candidates[candidate_index].estimated_fraction;
	}
	pfree(dominated);
	return selected_count;
}

static bool
bloompg_bitmap_candidates_preferred(BloomPGRelationState *state,
	BloomPGIndexCandidate *candidates, int count, int *selected,
	int *selected_count, int *dominated_count)
{
	static const double bitmap_min_heap_candidates = 8192.0;
	double estimated_rows;

	*selected_count = bloompg_select_bitmap_candidates(state, candidates,
		count, selected, dominated_count);
	if (*selected_count == 0)
		return false;
	estimated_rows = state->base_rows *
		candidates[selected[0]].estimated_fraction;
	return estimated_rows >= bitmap_min_heap_candidates ||
		(*selected_count > 1 && estimated_rows >= 1024.0);
}

static TIDBitmap *
bloompg_build_candidate_bitmap(Snapshot snapshot,
	BloomPGIndexCandidate *candidate, uint64 *keys_probed,
	uint64 *batches, uint64 *index_tuples, uint64 key_stride)
{
	BloomPGFilter *filter = candidate->sample_filter->filter;
	Relation index_relation = NULL;
	IndexScanDesc index_scan = NULL;
	TIDBitmap *bitmap = NULL;
	ScanKeyData scan_key;
	Datum *batch_values = NULL;
	int batch_count = 0;
	uint64 word_count;
	uint64 key_ordinal = 0;
	Size bitmap_bytes = Max(mul_size((Size) Max(work_mem, 1), (Size) 1024),
		(Size) 1024 * 1024);

	key_stride = Max(key_stride, UINT64CONST(1));

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
	batch_values = palloc_array(Datum, bloompg_index_transfer_batch_keys);
	PG_TRY();
	{
		uint64 word_index;

		index_scan = index_beginscan_bitmap(index_relation, snapshot, NULL, 1);
		word_count = filter->nbits / 64;
		for (word_index = 0; word_index < word_count; word_index++)
		{
			uint64 word = filter->words[word_index];

			while (word != 0)
			{
				int bit = pg_rightmost_one_pos64(word);
				uint64 bit_offset = word_index * 64 + (uint64) bit;
				int64 value = filter->bitmap_lower + (int64) bit_offset;
				Datum argument;

				word &= word - 1;
				if (key_ordinal++ % key_stride != 0)
					continue;
				if (value > filter->bitmap_upper ||
					!bloompg_integral_scan_datum(value,
						filter->keys[0].build_type, &argument))
					continue;
				batch_values[batch_count++] = argument;
				if (batch_count == bloompg_index_transfer_batch_keys)
				{
					bloompg_bitmap_array_batch(index_scan, &scan_key, bitmap,
						batch_values, batch_count,
						filter->keys[0].build_type, index_tuples);
					*keys_probed += (uint64) batch_count;
					(*batches)++;
					batch_count = 0;
				}
			}
		}
		if (batch_count > 0)
		{
			bloompg_bitmap_array_batch(index_scan, &scan_key, bitmap,
				batch_values, batch_count, filter->keys[0].build_type,
				index_tuples);
			*keys_probed += (uint64) batch_count;
			(*batches)++;
		}
	}
	PG_FINALLY();
	{
		if (index_scan != NULL)
			index_endscan(index_scan);
		if (batch_values != NULL)
			pfree(batch_values);
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
		&candidates);
	if (count == 0)
		return false;
	selected = palloc_array(int, count);
	if (!bloompg_bitmap_candidates_preferred(state, candidates, count,
			selected, &selected_count, &dominated_count))
	{
		pfree(selected);
		pfree(candidates);
		return false;
	}
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
				&source_index_tuples, 1);

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
	state->index_probe_fraction = primary_candidate->estimated_fraction;
	state->index_probe_oid = primary_candidate->index_oid;
	state->index_probe_attribute =
		primary_candidate->sample_filter->attributes[0];
	state->index_probe_filter_token = primary_candidate->sample_filter->token;
	state->index_probe_candidates += primary_candidate->candidates;
	state->index_probe_sample_rows += primary_candidate->sample_rows;
	state->index_probe_sample_matches += primary_candidate->sample_matches;
	state->index_probe_estimate_ms += primary_candidate->estimate_ms;
	state->index_probe_estimate_source = primary_candidate->estimate_source;
	state->index_bitmap_scans++;
	state->index_bitmap_sources += (uint64) built_sources;
	state->index_bitmap_index_tuples += index_tuples;
	state->index_bitmap_exact_pages += exact_pages;
	state->index_bitmap_lossy_pages += lossy_pages;
	state->index_probe_dominated_candidates += (uint64) dominated_count;
	state->index_bitmap_input_count = Min(built_sources,
		BLOOMPG_MAX_INDEX_BITMAP_SOURCES);
	state->materialization_access_path = "bitmap-index-probe";
	pfree(selected);
	pfree(candidates);
	return true;
}

bool
bloompg_index_prefilter_available(BloomPGRelationState *state)
{
	BloomPGIndexCandidate candidate;
	Relation	heap_relation;
	bool		available;

	if (state == NULL || state->table == NULL ||
		state->table->rte_kind != RTE_RELATION ||
		!OidIsValid(state->table->relation_oid))
		return false;
	heap_relation = table_open(state->table->relation_oid, AccessShareLock);
	available = bloompg_find_index_candidate(heap_relation, state, &candidate,
		NULL);
	table_close(heap_relation, AccessShareLock);
	return available;
}

bool
bloompg_index_prefilter_estimate(BloomPGRelationState *state,
	double *fraction, uint64 *keys, Oid *index_oid)
{
	BloomPGIndexCandidate candidate;
	Relation heap_relation;
	bool available;

	if (state == NULL || state->table == NULL ||
		state->table->rte_kind != RTE_RELATION ||
		!OidIsValid(state->table->relation_oid))
		return false;
	heap_relation = table_open(state->table->relation_oid, AccessShareLock);
	available = bloompg_find_index_candidate(heap_relation, state, &candidate,
		NULL);
	table_close(heap_relation, AccessShareLock);
	if (!available)
		return false;
	if (fraction != NULL)
		*fraction = candidate.estimated_fraction;
	if (keys != NULL)
		*keys = candidate.distinct_keys;
	if (index_oid != NULL)
		*index_oid = candidate.index_oid;
	return true;
}

/* Count the heap pages addressed by the current exact-key bitmap.  This is a
 * bounded physical probe: unlike output cardinality or planner cost, it tells
 * the transfer scheduler whether an index still avoids most of the heap. */
bool
bloompg_index_prefilter_page_fraction(BloomPGRelationState *state,
	double *fraction)
{
	BloomPGIndexCandidate *candidates = NULL;
	TIDBitmap *combined = NULL;
	Relation heap_relation;
	TBMIterator iterator;
	TBMIterateResult result;
	int *selected = NULL;
	int count;
	int selected_count;
	int ignored_dominated_count;
	uint64 page_count = 0;
	BlockNumber heap_pages;
	bool iterator_started = false;
	bool available = false;
	uint64 keys_probed = 0;
	uint64 ignored_batches = 0;
	uint64 ignored_index_tuples = 0;

	if (fraction != NULL)
		*fraction = 0.0;
	if (state == NULL || state->table == NULL ||
		state->table->rte_kind != RTE_RELATION ||
		!OidIsValid(state->table->relation_oid) || !ActiveSnapshotSet())
		return false;
	heap_relation = table_open(state->table->relation_oid, AccessShareLock);
	PG_TRY();
	{
		count = bloompg_collect_index_candidates(heap_relation, state,
			&candidates);
		if (count > 0)
		{
			selected = palloc_array(int, count);
			if (bloompg_bitmap_candidates_preferred(state, candidates, count,
					selected, &selected_count, &ignored_dominated_count) &&
				selected_count == 1)
			{
				BloomPGIndexCandidate *candidate =
					&candidates[selected[0]];
				uint64 key_stride = (candidate->distinct_keys +
					BLOOMPG_INDEX_PAGE_PROBE_KEYS - 1) /
					BLOOMPG_INDEX_PAGE_PROBE_KEYS;

				combined = bloompg_build_candidate_bitmap(GetActiveSnapshot(),
					candidate, &keys_probed, &ignored_batches,
					&ignored_index_tuples, key_stride);
				available = combined != NULL;
			}
		}
		if (available && !tbm_is_empty(combined))
		{
			iterator = tbm_begin_iterate(combined, NULL,
				InvalidDsaPointer);
			iterator_started = true;
			while (tbm_iterate(&iterator, &result))
			{
				CHECK_FOR_INTERRUPTS();
				page_count++;
			}
		}
		heap_pages = RelationGetNumberOfBlocks(heap_relation);
		if (fraction != NULL && heap_pages > 0 && keys_probed > 0)
		{
			double sampled_fraction = Min((double) page_count /
				(double) heap_pages, 1.0);
			double key_scale = (double)
				candidates[selected[0]].distinct_keys / (double) keys_probed;

			*fraction = sampled_fraction >= 1.0 ? 1.0 :
				1.0 - pow(1.0 - sampled_fraction, key_scale);
		}
	}
	PG_FINALLY();
	{
		if (iterator_started && !tbm_exhausted(&iterator))
			tbm_end_iterate(&iterator);
		if (combined != NULL)
			tbm_free(combined);
		if (selected != NULL)
			pfree(selected);
		if (candidates != NULL)
			pfree(candidates);
		table_close(heap_relation, AccessShareLock);
	}
	PG_END_TRY();
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
	BloomPGIndexCandidate candidate;
	BloomPGFilter *filter;
	Relation	index_relation = NULL;
	IndexScanDesc index_scan = NULL;
	TupleTableSlot *slot;
	ScanKeyData scan_key;
	instr_time	started;
	uint64		word_count;
	uint64		keys_probed = 0;
	uint64		batches = 0;
	uint64		rows_fetched = 0;
	bool		keep_scanning = true;
	Datum	   *batch_values;
	int			batch_count = 0;
	int			dominated_count = 0;

	if (bloompg_bitmap_prefilter_scan(state, heap_relation, snapshot,
			callback, callback_context))
		return true;

	if (!bloompg_find_index_candidate(heap_relation, state, &candidate,
			&dominated_count))
		return false;
	filter = candidate.sample_filter->filter;
	index_relation = index_open(candidate.index_oid, AccessShareLock);
	if (!bloompg_index_matches_filter(index_relation,
			candidate.sample_filter, candidate.equality_operator))
	{
		index_close(index_relation, AccessShareLock);
		return false;
	}
	slot = table_slot_create(heap_relation, NULL);
	ScanKeyEntryInitialize(&scan_key, SK_SEARCHARRAY, 1, BTEqualStrategyNumber,
		filter->keys[0].build_type, index_relation->rd_indcollation[0],
		get_opcode(candidate.equality_operator), (Datum) 0);
	batch_values = palloc_array(Datum, bloompg_index_transfer_batch_keys);
	INSTR_TIME_SET_CURRENT(started);
	PG_TRY();
	{
		uint64 word_index;

		index_scan = index_beginscan(heap_relation, index_relation, snapshot,
			NULL, 1, 0);
		word_count = filter->nbits / 64;
		for (word_index = 0; word_index < word_count && keep_scanning;
			 word_index++)
		{
			uint64 word = filter->words[word_index];

			while (word != 0 && keep_scanning)
			{
				int bit = pg_rightmost_one_pos64(word);
				uint64 bit_offset = word_index * 64 + (uint64) bit;
				int64 value = filter->bitmap_lower + (int64) bit_offset;
				Datum argument;

				word &= word - 1;
				if (value > filter->bitmap_upper ||
					!bloompg_integral_scan_datum(value,
						filter->keys[0].build_type, &argument))
					continue;
				batch_values[batch_count++] = argument;
				if (batch_count == bloompg_index_transfer_batch_keys)
				{
					keep_scanning = bloompg_probe_array_batch(index_scan,
						&scan_key, slot, batch_values, batch_count,
						filter->keys[0].build_type, callback,
						callback_context, &rows_fetched);
					keys_probed += (uint64) batch_count;
					batches++;
					batch_count = 0;
				}
			}
		}
		if (batch_count > 0 && keep_scanning)
		{
			keep_scanning = bloompg_probe_array_batch(index_scan, &scan_key,
				slot, batch_values, batch_count, filter->keys[0].build_type,
				callback, callback_context, &rows_fetched);
			keys_probed += (uint64) batch_count;
			batches++;
			batch_count = 0;
		}
	}
	PG_FINALLY();
	{
		if (index_scan != NULL)
			index_endscan(index_scan);
		pfree(batch_values);
		ExecDropSingleTupleTableSlot(slot);
		index_close(index_relation, AccessShareLock);
	}
	PG_END_TRY();
	state->index_probe_scans++;
	state->index_probe_batches += batches;
	state->index_probe_keys += keys_probed;
	state->index_probe_rows += rows_fetched;
	state->index_probe_ms += bloompg_transfer_elapsed_ms(started);
	state->index_probe_fraction = candidate.estimated_fraction;
	state->index_probe_oid = candidate.index_oid;
	state->index_probe_attribute = candidate.sample_filter->attributes[0];
	state->index_probe_filter_token = candidate.sample_filter->token;
	state->index_probe_candidates += candidate.candidates;
	state->index_probe_sample_rows += candidate.sample_rows;
	state->index_probe_sample_matches += candidate.sample_matches;
	state->index_probe_estimate_ms += candidate.estimate_ms;
	state->index_probe_estimate_source = candidate.estimate_source;
	state->index_probe_dominated_candidates += (uint64) dominated_count;
	state->materialization_access_path = "index-probe";
	return true;
}
