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

/* Exact filter-derived and post-transfer cardinality statistics. */

typedef struct BloomPGBuiltKeyStatistics
{
	int			nkeys;
	AttrNumber *materialized_attributes;
	Oid		   *operators;
	Oid		   *collations;
	uint64		rows;
	uint64		nonnull_rows;
	uint64		null_rows;
	double		ndistinct;
} BloomPGBuiltKeyStatistics;

static bool
bloompg_built_key_matches(const BloomPGBuiltKeyStatistics *statistics,
	const BloomPGApplySpec *spec)
{
	int key;

	if (statistics->nkeys != spec->nkeys)
		return false;
	for (key = 0; key < spec->nkeys; key++)
	{
		if (statistics->materialized_attributes[key] !=
			spec->build_attributes[key] ||
			statistics->operators[key] != spec->operators[key] ||
			statistics->collations[key] != spec->collations[key])
			return false;
	}
	return true;
}

/* An exact bitmap already is the final key-domain statistic needed by P1.
 * Copy its cardinality metadata while the filter is available, avoiding a
 * later HLL pass over the same selected vectors. */
void
bloompg_remember_built_key_statistics(BloomPGRelationState *state,
	BloomPGPendingTransfer *pending, uint64 rows)
{
	BloomPGBuiltKeyStatistics *statistics = NULL;
	ListCell *cell;
	uint64 nonnull_rows;
	uint64 null_rows;
	double ndistinct;

	if (!pending->build_filter)
		return;
	if (pending->integral_key && !pending->has_range)
	{
		pending->exact_ndistinct_valid = true;
		pending->exact_ndistinct = 0;
	}
	if (!bloompg_filter_exact_statistics(pending->filter, rows,
		&nonnull_rows, &null_rows, &ndistinct))
		return;
	pending->exact_ndistinct_valid = true;
	pending->exact_ndistinct = (uint64) ndistinct;
	foreach(cell, state->built_key_statistics)
	{
		BloomPGBuiltKeyStatistics *candidate = lfirst(cell);

		if (bloompg_built_key_matches(candidate, &pending->spec))
		{
			statistics = candidate;
			break;
		}
	}
	if (statistics == NULL)
	{
		int bytes = sizeof(AttrNumber) * pending->spec.nkeys;

		statistics = palloc0(sizeof(BloomPGBuiltKeyStatistics));
		statistics->nkeys = pending->spec.nkeys;
		statistics->materialized_attributes =
			palloc_array(AttrNumber, statistics->nkeys);
		statistics->operators = palloc_array(Oid, statistics->nkeys);
		statistics->collations = palloc_array(Oid, statistics->nkeys);
		memcpy(statistics->materialized_attributes,
			pending->spec.build_attributes, bytes);
		memcpy(statistics->operators, pending->spec.operators,
			sizeof(Oid) * statistics->nkeys);
		memcpy(statistics->collations, pending->spec.collations,
			sizeof(Oid) * statistics->nkeys);
		state->built_key_statistics = lappend(state->built_key_statistics,
			statistics);
	}
	statistics->rows = rows;
	statistics->nonnull_rows = nonnull_rows;
	statistics->null_rows = null_rows;
	statistics->ndistinct = ndistinct;
}

static BloomPGBuiltKeyStatistics *
bloompg_find_built_key_statistics(BloomPGRelationState *state,
	const BloomPGJoinKeyStatistics *keys)
{
	ListCell *cell;

	foreach(cell, state->built_key_statistics)
	{
		BloomPGBuiltKeyStatistics *statistics = lfirst(cell);
		int key;
		bool matches = statistics->nkeys == keys->nkeys;

		for (key = 0; matches && key < keys->nkeys; key++)
		{
			matches = statistics->materialized_attributes[key] ==
				keys->materialized_attributes[key] &&
				statistics->operators[key] == keys->operators[key] &&
				statistics->collations[key] == keys->collations[key];
		}
		if (matches && statistics->rows == (uint64) state->current_rows)
			return statistics;
	}
	return NULL;
}

static bool
bloompg_statistics_edge_for_state(const BloomPGJoinEdge *edge,
	BloomPGRelationState *state, BloomPGRelationState **states,
	int relation_count, BloomPGColumnRef *column,
	BloomPGRelationState **peer, bool *state_is_left)
{
	Index peer_rti;

	if (edge->join_type != JOIN_INNER || edge->protect_left ||
		edge->protect_right)
		return false;
	if (edge->left.rti == state->rti)
	{
		*column = edge->left;
		peer_rti = edge->right.rti;
		*state_is_left = true;
	}
	else if (edge->right.rti == state->rti)
	{
		*column = edge->right;
		peer_rti = edge->left.rti;
		*state_is_left = false;
	}
	else
		return false;
	if (peer_rti == 0 || peer_rti > (Index) relation_count ||
		states[peer_rti] == NULL || !states[peer_rti]->materialized ||
		!states[peer_rti]->changed)
		return false;
	*peer = states[peer_rti];
	return (*peer)->replacement_rti != state->replacement_rti;
}

static BloomPGJoinKeyStatistics *
bloompg_find_statistics_peer(BloomPGMaterializedStatistics *statistics,
	Index peer_rti)
{
	ListCell *cell;

	foreach(cell, statistics->join_keys)
	{
		BloomPGJoinKeyStatistics *keys = lfirst(cell);

		if (keys->peer_rti == peer_rti)
			return keys;
	}
	return NULL;
}

/*
 * Seal one materialization's post-transfer statistics before P1 starts.  The
 * scan stays on selected column vectors; spilled stores retain the equivalent
 * slot fallback.  Equality columns shared with one peer are collected as one
 * composite key, preserving their correlation for join cardinality.
 */
BloomPGMaterializedStatistics *
bloompg_finalize_state_statistics(BloomPGRelationState *state,
	BloomPGRelationState **states, int relation_count,
	const BloomPGJoinGraph *graph,
	BloomPGRuntimeMaterializationEntry *entry)
{
	BloomPGMaterializedStatistics *statistics;
	ListCell *cell;

	Assert(state != NULL && state->materialized);
	Assert(entry != NULL && entry->store != NULL && entry->descriptor != NULL);
	statistics = palloc0(sizeof(BloomPGMaterializedStatistics));
	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *edge = lfirst(cell);
		BloomPGColumnRef ignored_column;
		BloomPGRelationState *peer;
		bool ignored_left;
		BloomPGJoinKeyStatistics *keys;
		ListCell *key_cell;
		int key_count = 0;
		int key_index = 0;
		bool valid = true;

		if (!bloompg_statistics_edge_for_state(edge, state, states,
			relation_count, &ignored_column, &peer, &ignored_left) ||
			bloompg_find_statistics_peer(statistics,
				peer->replacement_rti) != NULL)
			continue;
		foreach(key_cell, graph->edges)
		{
			BloomPGJoinEdge *candidate = lfirst(key_cell);
			BloomPGColumnRef column;
			BloomPGRelationState *candidate_peer;
			bool state_is_left;

			if (bloompg_statistics_edge_for_state(candidate, state, states,
				relation_count, &column, &candidate_peer, &state_is_left) &&
				candidate_peer->replacement_rti == peer->replacement_rti)
				key_count++;
		}
		if (key_count == 0)
			continue;
		keys = palloc0(sizeof(BloomPGJoinKeyStatistics));
		keys->peer_rti = peer->replacement_rti;
		keys->nkeys = key_count;
		keys->base_attributes = palloc_array(AttrNumber, key_count);
		keys->materialized_attributes = palloc_array(AttrNumber, key_count);
		keys->operators = palloc_array(Oid, key_count);
		keys->collations = palloc_array(Oid, key_count);
		keys->hash_procs = palloc_array(Oid, key_count);
		foreach(key_cell, graph->edges)
		{
			BloomPGJoinEdge *candidate = lfirst(key_cell);
			BloomPGColumnRef column;
			BloomPGRelationState *candidate_peer;
			bool state_is_left;
			RegProcedure left_hash;
			RegProcedure right_hash;

			if (!bloompg_statistics_edge_for_state(candidate, state, states,
				relation_count, &column, &candidate_peer, &state_is_left) ||
				candidate_peer->replacement_rti != peer->replacement_rti)
				continue;
			keys->base_attributes[key_index] = column.attribute;
			keys->materialized_attributes[key_index] =
				bloompg_find_state_output_attribute(state, column);
			keys->operators[key_index] = candidate->operator_oid;
			keys->collations[key_index] = candidate->input_collation;
			if (!AttributeNumberIsValid(
					keys->materialized_attributes[key_index]) ||
				!get_op_hash_functions(candidate->operator_oid,
					&left_hash, &right_hash))
			{
				valid = false;
				break;
			}
			keys->hash_procs[key_index] = state_is_left ?
				left_hash : right_hash;
			key_index++;
		}
		if (valid && key_index == key_count)
		{
			BloomPGBuiltKeyStatistics *built =
				bloompg_find_built_key_statistics(state, keys);

			if (built != NULL)
			{
				keys->nonnull_rows = built->nonnull_rows;
				keys->null_rows = built->null_rows;
				keys->ndistinct = built->ndistinct;
				keys->precomputed = true;
			}
			statistics->join_keys = lappend(statistics->join_keys, keys);
		}
	}
	bloompg_statistics_finalize(statistics, entry->store, entry->descriptor);
	return statistics;
}
