#ifndef BLOOMPG_H
#define BLOOMPG_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "optimizer/planner.h"

#define BLOOMPG_VERSION "0.1.2"
#define BLOOMPG_VERSION_STRING \
	"bloompg " BLOOMPG_VERSION " (PostgreSQL 18)"

typedef enum BloomPGTransferProgressMetric
{
	BLOOMPG_TRANSFER_PROGRESS_ROWS,
	BLOOMPG_TRANSFER_PROGRESS_NDV
} BloomPGTransferProgressMetric;

extern bool bloompg_enabled;
extern bool bloompg_log_transfer_steps;
extern bool bloompg_profile_enabled;
extern bool bloompg_profile_log;
extern bool bloompg_replan_after_transfer;
extern bool bloompg_late_materialize;
extern int bloompg_late_materialize_min_width;
extern int bloompg_transfer_workers;
extern int bloompg_transfer_parallel_min_rows;
extern bool bloompg_index_transfer;
extern double bloompg_index_transfer_max_fraction;
extern int bloompg_index_transfer_batch_keys;
extern int bloompg_index_transfer_max_keys;
extern int bloompg_index_transfer_planner_max_keys;
extern int bloompg_index_guard_min_rows;
extern int bloompg_transfer_progress_metric;
extern int bloompg_sample_size;
extern int bloompg_sample_seed;
extern double bloompg_sample_rate;
extern int bloompg_bits_per_key;
extern int bloompg_max_filter_bytes;
extern int bloompg_materialization_memory_kb;
extern double bloompg_excitation_threshold;
extern double bloompg_local_first_fraction;
extern char *bloompg_sample_mode;
extern char *bloompg_sample_cache_dir;
extern bool bloompg_sample_memory_cache;

extern void bloompg_planner_init(void);
extern void bloompg_planner_fini(void);
extern void bloompg_scan_init(void);
extern void bloompg_replan_init(void);
extern void bloompg_replan_fini(void);
extern void bloompg_sampling_init(void);
extern void bloompg_sampling_fini(void);
extern void bloompg_runtime_init(void);
extern void bloompg_runtime_fini(void);

extern PlannedStmt *bloompg_planner(Query *parse,
									const char *query_string,
									int cursor_options,
									ParamListInfo bound_params);

extern const char *bloompg_get_last_trace(void);
extern void bloompg_set_last_trace(const char *trace);

#endif
