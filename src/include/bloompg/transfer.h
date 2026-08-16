#ifndef BLOOMPG_TRANSFER_H
#define BLOOMPG_TRANSFER_H

#include "postgres.h"

#include "bloompg/graph.h"
#include "bloompg/statistics.h"
#include "nodes/params.h"
#include "nodes/plannodes.h"
#include "utils/resowner.h"

typedef enum BloomPGReplacementKind
{
	BLOOMPG_REPLACEMENT_MEMORY,
	BLOOMPG_REPLACEMENT_TID,
	BLOOMPG_REPLACEMENT_FILTER,
	BLOOMPG_REPLACEMENT_EMPTY
} BloomPGReplacementKind;

typedef struct BloomPGReplacement
{
	BloomPGReplacementKind kind;
	Index		rti;
	uint64		token;
	uint64		fallback_token;
	double		rows;
	List	   *targetlist;
	List	   *filters;
	/* A native parameterized path is an equivalent alternative to the exact
	 * materialization only when the materialization consumed no incoming
	 * Bloom filter.  This lets P1 retain keyed lookup without weakening the
	 * relation-replacement contract. */
	bool		native_parameterized_paths_equivalent;
	BloomPGMaterializedStatistics *statistics;
} BloomPGReplacement;

typedef struct BloomPGTransferOutcome
{
	bool		applied;
	/* The transfer scheduler reached a state where every remaining large
	 * source was waiting for an index-driving exact key set.  The caller must
	 * discard optimizer-time artifacts and execute the native preview plan. */
	bool		native_fallback;
	const char *native_fallback_reason;
	bool		analysis_plan_modified;
	int			steps;
	int			filters;
	uint64		build_rows;
	double		statistics_ms;
	uint64		statistics_rows;
	uint64		statistics_values;
	int			statistics_key_sets;
	uint64		equality_domain_classes;
	uint64		equality_domain_tracked;
	uint64		equality_domain_updates;
	uint64		equality_domain_noops;
	uint64		equality_domain_suppressed;
	uint64		equality_domain_observations;
	uint64		equality_domain_uncontained;
	double		equality_domain_ms;
	char	   *detail;
	char	   *profile_json;
	List	   *replacements;
} BloomPGTransferOutcome;

extern BloomPGTransferOutcome bloompg_apply_transfers(PlannedStmt *candidate,
											 Query *analysis_query,
											 BloomPGJoinGraph *graph,
											 const char *query_string,
											 int cursor_options,
											 ParamListInfo bound_params,
											 ResourceOwner runtime_owner,
											 bool query_parallel_safe,
											 const Bitmapset *native_parameterized_indexes);
extern Bitmapset *bloompg_parameterized_index_relids(PlannedStmt *statement);
extern void bloompg_finish_replacements(List *replacements,
										PlannedStmt *final_plan);

#endif
