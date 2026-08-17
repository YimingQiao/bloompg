#ifndef BLOOMPG_STATISTICS_H
#define BLOOMPG_STATISTICS_H

#include "postgres.h"

#include "bloompg/column_store.h"
#include "nodes/pg_list.h"

/*
 * Statistics for one equality-key vector shared by two relations.  A key can
 * contain several columns; estimating it as one composite value avoids the
 * severe underestimates caused by multiplying correlated per-column
 * selectivities.
 *
 * The arrays describe this relation's side of the equality.  Hash procedures
 * are chosen from the equality operators, so cross-type hash joins retain
 * PostgreSQL's compatibility semantics.
 */
typedef struct BloomPGJoinKeyStatistics
{
	Index		peer_rti;
	int			nkeys;
	AttrNumber *base_attributes;
	AttrNumber *materialized_attributes;
	Oid		   *operators;
	Oid		   *collations;
	Oid		   *hash_procs;

	uint64		nonnull_rows;
	uint64		null_rows;
	double		ndistinct;
	bool		precomputed;
} BloomPGJoinKeyStatistics;

typedef struct BloomPGMaterializedStatistics
{
	uint64		rows;
	uint64		rows_examined;
	uint64		values_examined;
	int32		average_width;
	double		finalize_ms;
	bool		width_estimated;
	int		precomputed_key_sets;
	int		scanned_key_sets;
	List	   *join_keys;
} BloomPGMaterializedStatistics;

extern void bloompg_statistics_finalize(
	BloomPGMaterializedStatistics *statistics,
	BloomPGColumnStore *store,
	TupleDesc descriptor);

#endif
