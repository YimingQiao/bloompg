#ifndef BLOOMPG_PROFILE_H
#define BLOOMPG_PROFILE_H

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"

typedef struct BloomPGPlanProfileInput
{
	const char *transfer_trace;
	const char *transfer_json;
	const char *p1_path_transform_json;
	double		planning_ms;
	double		p0_ms;
	double		native_preview_ms;
	double		graph_ms;
	double		transfer_ms;
	double		statistics_ms;
	double		p1_ms;
	uint64		p1_cardinality_adjustments;
	double		p1_cardinality_max_ratio;
	bool		replanned;
	bool		p1_nestloop_suppressed;
	bool		p1_parallel_biased;
	bool		transfer_failed;
	int		transfer_error_code;
	List	   *materialization_tokens;
} BloomPGPlanProfileInput;

extern void bloompg_profile_init(void);
extern void bloompg_profile_fini(void);
extern void bloompg_profile_remember_plan(PlannedStmt *statement,
	const char *query_string, const BloomPGPlanProfileInput *input);
extern const char *bloompg_profile_last_json(void);

#endif
