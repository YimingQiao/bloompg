#ifndef BLOOMPG_SAMPLING_H
#define BLOOMPG_SAMPLING_H

#include "postgres.h"

#include "bloompg/filter.h"
#include "nodes/bitmapset.h"
#include "nodes/plannodes.h"

typedef struct BloomPGSampleFilter
{
	uint64		token;
	BloomPGFilter *filter;
	int			nkeys;
	AttrNumber *attributes;
	AttrNumber *materialized_attributes;
	Bitmapset  *lineage;
} BloomPGSampleFilter;

/* Query-local, predicate-qualified view of a prepared TID sample.  Persistent
 * files contain CTIDs only; the optional backend tuple cache is a relaxed
 * planning estimate and never substitutes for an exact execution scan. */
typedef struct BloomPGSampleWorkspace BloomPGSampleWorkspace;

typedef struct BloomPGSamplingProfile
{
	uint64		acquisitions;
	uint64		memory_hits;
	uint64		disk_hits;
	uint64		builds;
	double		acquire_ms;
	double		build_ms;
} BloomPGSamplingProfile;

/* Sampling telemetry for a relation that already lives in Bloom's query-local
 * column store.  Unlike the persistent base-table sample, this sample and its
 * exact cardinality denominator describe the same current row set. */
typedef struct BloomPGIntermediateSamplingProfile
{
	uint64		calls;
	uint64		rows_examined;
	uint64		rows_passed;
	double		probe_ms;
	double		last_fraction;
} BloomPGIntermediateSamplingProfile;

extern double bloompg_sample_relation(PlannedStmt *candidate,
									  Index rti,
									  Plan *scan,
									  double base_rows,
									  const char *query_string,
									  ParamListInfo bound_params,
									  BloomPGSampleWorkspace **workspace,
									  BloomPGSamplingProfile *profile,
									  bool *sampled,
									  bool *cache_hit);
extern double bloompg_sample_filtered_relation(PlannedStmt *candidate,
										Index rti,
										Plan *scan,
										double base_rows,
										List *filters,
										const char *query_string,
										ParamListInfo bound_params,
										BloomPGSampleWorkspace **workspace,
										BloomPGSamplingProfile *profile,
										bool *sampled,
										bool *cache_hit,
										double *filter_fraction);
extern double bloompg_sample_materialized_relation(BloomPGColumnStore *store,
	TupleDesc descriptor, List *filters,
	BloomPGIntermediateSamplingProfile *profile);
extern void bloompg_sample_workspace_stats(BloomPGSampleWorkspace *workspace,
										   uint64 *visible_rows,
									   uint64 *local_rows,
									   double *load_ms,
									   double *hydrate_ms,
									   double *qual_ms,
									   bool *tuple_cache_hit,
										   uint64 *probe_count,
										   double *probe_ms,
										   uint64 *probe_rows_examined,
										   uint64 *probe_filter_checks,
										   uint64 *probe_filters_applied,
										   uint64 *probe_cache_hits);
extern bool bloompg_sample_workspace_order_costs(
	BloomPGSampleWorkspace *workspace, double *local_sample_ms,
	double *filter_sample_ms);
extern bool bloompg_sample_workspace_filter_fraction(
	BloomPGSampleWorkspace *workspace, BloomPGSampleFilter *filter,
	double *fraction, uint64 *rows_examined, uint64 *rows_matched,
	double *probe_ms);
extern void bloompg_sample_workspace_destroy(BloomPGSampleWorkspace *workspace);
extern void bloompg_sampling_init(void);
extern void bloompg_sampling_fini(void);

#endif
