#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/profile.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/jsonb.h"

#ifndef WIN32
#include <unistd.h>
#endif

#include <errno.h>

PG_MODULE_MAGIC;

bool bloompg_enabled = true;
bool bloompg_log_transfer_steps = false;
bool bloompg_profile_enabled = false;
bool bloompg_profile_log = true;
bool bloompg_replan_after_transfer = true;
bool bloompg_p1_disable_nestloop = false;
bool bloompg_p1_parallel_bias = true;
int bloompg_p1_parallel_min_rows = 10000;
bool bloompg_late_materialize = true;
int bloompg_late_materialize_min_width = 128;
int bloompg_transfer_workers = 4;
int bloompg_transfer_parallel_min_rows = 100000;
bool bloompg_index_transfer = true;
double bloompg_index_transfer_max_fraction = 0.15;
int bloompg_index_transfer_batch_keys = 65536;
int bloompg_index_transfer_max_keys = 100000;
int bloompg_index_transfer_planner_max_keys = 8192;
int bloompg_index_guard_min_rows = 100000;
int bloompg_transfer_progress_metric = BLOOMPG_TRANSFER_PROGRESS_NDV;
int bloompg_sample_size = 10000;
int bloompg_sample_seed = 2;
double bloompg_sample_rate = 0.01;
int bloompg_bits_per_key = 24;
int bloompg_max_filter_bytes = 64 * 1024 * 1024;
int bloompg_materialization_memory_kb = 512 * 1024;
double bloompg_excitation_threshold = 1.0;
double bloompg_local_first_fraction = 0.001;
char *bloompg_sample_mode = NULL;
char *bloompg_sample_cache_dir = NULL;
bool bloompg_sample_memory_cache = true;

static const struct config_enum_entry bloompg_transfer_progress_options[] = {
	{"rows", BLOOMPG_TRANSFER_PROGRESS_ROWS, false},
	{"ndv", BLOOMPG_TRANSFER_PROGRESS_NDV, false},
	{NULL, 0, false}
};

void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

PG_FUNCTION_INFO_V1(bloompg_version);
PG_FUNCTION_INFO_V1(bloompg_last_trace);
PG_FUNCTION_INFO_V1(bloompg_last_profile);

static bool
bloompg_check_sample_mode(char **new_value, void **extra, GucSource source)
{
	(void) extra;
	(void) source;
	if (pg_strcasecmp(*new_value, "prepared") != 0 &&
		pg_strcasecmp(*new_value, "instant") != 0)
	{
		GUC_check_errdetail("Available values are \"prepared\" and \"instant\".");
		return false;
	}
	return true;
}

static bool
bloompg_read_memory_limit(const char *path, uint64 *result)
{
	FILE *file;
	char buffer[64];
	char *end;
	unsigned long long parsed;

	file = fopen(path, "r");
	if (file == NULL)
		return false;
	if (fgets(buffer, sizeof(buffer), file) == NULL)
	{
		fclose(file);
		return false;
	}
	fclose(file);
	end = buffer + strlen(buffer);
	while (end > buffer && isspace((unsigned char) end[-1]))
		*--end = '\0';
	if (strcmp(buffer, "max") == 0)
		return false;
	errno = 0;
	parsed = strtoull(buffer, &end, 10);
	while (isspace((unsigned char) *end))
		end++;
	if (errno != 0 || end == buffer || *end != '\0' || parsed == 0)
		return false;
	*result = (uint64) parsed;
	return true;
}

/* Materialization is optional optimizer work.  Give a single analytical query
 * useful headroom on a dedicated host, while keeping small machines and
 * containers proportional to their detected memory. */
static int
bloompg_default_materialization_memory_kb(void)
{
	uint64 usable_bytes = 0;
	uint64 cgroup_bytes;
	uint64 default_bytes;
	bool memory_known = false;

#ifdef _SC_PHYS_PAGES
	long		pages = sysconf(_SC_PHYS_PAGES);
	long		page_size = sysconf(_SC_PAGESIZE);

	if (pages > 0 && page_size > 0 &&
		(uint64) pages <= UINT64_MAX / (uint64) page_size)
	{
		usable_bytes = (uint64) pages * (uint64) page_size;
		memory_known = true;
	}
#endif
	if (bloompg_read_memory_limit("/sys/fs/cgroup/memory.max",
			&cgroup_bytes) ||
		bloompg_read_memory_limit(
			"/sys/fs/cgroup/memory/memory.limit_in_bytes", &cgroup_bytes))
	{
		/* cgroup v1 can use a near-UINT64 sentinel for "unlimited". */
		if (cgroup_bytes < (UINT64CONST(1) << 60) &&
			(!memory_known || cgroup_bytes < usable_bytes))
		{
			usable_bytes = cgroup_bytes;
			memory_known = true;
		}
	}
	default_bytes = memory_known ?
		Min(UINT64CONST(2) * 1024 * 1024 * 1024, usable_bytes / 8) :
		UINT64CONST(512) * 1024 * 1024;
	/* The GUC itself has a 64kB minimum. */
	default_bytes = Max(default_bytes, UINT64CONST(64) * 1024);
	return (int) Min(default_bytes / 1024, (uint64) INT_MAX);
}

void
_PG_init(void)
{
	int			materialization_default_kb =
		bloompg_default_materialization_memory_kb();

	/* Dynamic GUC boot values must match the backing C variable at define time. */
	bloompg_materialization_memory_kb = materialization_default_kb;

	DefineCustomBoolVariable("bloompg.enable",
							 "Enable Bloom filter-transfer planning.",
							 NULL,
							 &bloompg_enabled,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomBoolVariable("bloompg.log_transfer_steps",
							 "Log the Bloom excitation timeline.",
							 NULL,
							 &bloompg_log_transfer_steps,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomBoolVariable("bloompg.replan_after_transfer",
							 "Run a second PostgreSQL planning pass after Bloom replacements are ready.",
							 NULL,
							 &bloompg_replan_after_transfer,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
								 NULL,
								 NULL);
	DefineCustomBoolVariable("bloompg.profile",
							 "Collect structured Bloom planning and execution profiles.",
							 NULL,
							 &bloompg_profile_enabled,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomBoolVariable("bloompg.profile_log",
							 "Write completed structured Bloom profiles to the server log.",
							 NULL,
							 &bloompg_profile_log,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomBoolVariable("bloompg.p1_disable_nestloop",
								 "Emergency guard that disables P1 nested loops after transfer.",
								 NULL,
								 &bloompg_p1_disable_nestloop,
								 false,
								 PGC_USERSET,
								 0,
								 NULL,
								 NULL,
								 NULL);
	DefineCustomBoolVariable("bloompg.p1_parallel_bias",
								 "Use exact post-transfer rows to favor parallel P1 plans for substantial materializations.",
								 NULL,
								 &bloompg_p1_parallel_bias,
								 true,
								 PGC_USERSET,
								 0,
								 NULL,
								 NULL,
								 NULL);
	DefineCustomIntVariable("bloompg.p1_parallel_min_rows",
							"Minimum exact materialized rows before Bloom biases only the P1 optimizer toward parallel execution.",
							NULL,
							&bloompg_p1_parallel_min_rows,
							10000,
							0,
							INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);
	DefineCustomBoolVariable("bloompg.late_materialize",
							 "Prefer TID-backed late materialization for wide relations.",
							 NULL,
							 &bloompg_late_materialize,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomIntVariable("bloompg.late_materialize_min_width",
							"Minimum projected byte width for TID late materialization.",
							NULL,
							&bloompg_late_materialize_min_width,
							128,
							0,
							1024 * 1024,
							PGC_USERSET,
							GUC_UNIT_BYTE,
							NULL,
							NULL,
							NULL);
	DefineCustomIntVariable("bloompg.transfer_workers",
							"Maximum PostgreSQL parallel workers for an eligible transfer scan; zero disables transfer parallelism.",
							NULL,
							&bloompg_transfer_workers,
							4,
							0,
							128,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);
	DefineCustomIntVariable("bloompg.transfer_parallel_min_rows",
							"Minimum estimated base rows before Bloom uses parallel transfer scanning.",
							NULL,
							&bloompg_transfer_parallel_min_rows,
							100000,
							0,
							INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);
	DefineCustomBoolVariable("bloompg.index_transfer",
							 "Use exact transfer keys to drive matching B-tree indexes.",
							 NULL,
							 &bloompg_index_transfer,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomRealVariable("bloompg.index_transfer_max_fraction",
							 "Maximum sampled survivor fraction for exact-key B-tree transfer scans.",
							 NULL,
							 &bloompg_index_transfer_max_fraction,
							 0.15,
							 0.0,
							 1.0,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomIntVariable("bloompg.index_transfer_batch_keys",
							 "Exact keys in one PostgreSQL B-tree search-array batch.",
							 NULL,
							 &bloompg_index_transfer_batch_keys,
							 65536,
							 1,
							 10000000,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomIntVariable("bloompg.index_transfer_max_keys",
							 "Maximum exact-key population eligible for an optimizer-time B-tree transfer scan.",
							 NULL,
							 &bloompg_index_transfer_max_keys,
							 100000,
							 1,
							 1000000000,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomIntVariable("bloompg.index_transfer_planner_max_keys",
							 "Maximum total exact keys expanded into one private PostgreSQL transfer access plan.",
							 "Larger exact sets retain the batched direct-probe path to avoid excessive planner CPU.",
							 &bloompg_index_transfer_planner_max_keys,
							 8192,
							 1,
							 1000000000,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomIntVariable("bloompg.index_guard_min_rows",
							 "Minimum base-relation rows before a relation with a join-key index waits for exact transfer keys.",
							 NULL,
							 &bloompg_index_guard_min_rows,
							 100000,
							 0,
							 INT_MAX,
							 PGC_USERSET,
							 0,
								 NULL,
								 NULL,
								 NULL);
	DefineCustomEnumVariable("bloompg.transfer_progress_metric",
							 "Choose the equality-transfer convergence signal.",
							 "\"rows\" retains row-cardinality/lineage propagation; \"ndv\" also suppresses exact equality-domain no-ops.",
							 &bloompg_transfer_progress_metric,
							 BLOOMPG_TRANSFER_PROGRESS_NDV,
							 bloompg_transfer_progress_options,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomIntVariable("bloompg.sample_size",
							"Target rows in each base-relation sample.",
							NULL,
							&bloompg_sample_size,
							10000,
							1,
							1000000000,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);
	DefineCustomIntVariable("bloompg.sample_seed",
							"Deterministic seed for prepared base-relation samples.",
							NULL,
							&bloompg_sample_seed,
							2,
							0,
							INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);
	DefineCustomRealVariable("bloompg.sample_rate",
							 "Chunk-sampling fraction for already-materialized intermediate data.",
							 NULL,
							 &bloompg_sample_rate,
							 0.01,
							 0.0,
							 1.0,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomBoolVariable("bloompg.sample_memory_cache",
							 "Keep prepared TID samples in backend memory.",
							 NULL,
							 &bloompg_sample_memory_cache,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomStringVariable("bloompg.sample_cache_dir",
							   "Prepared TID-sample cache directory; auto uses PGDATA/bloompg_samples.",
							   NULL,
							   &bloompg_sample_cache_dir,
							   "auto",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);
	DefineCustomRealVariable("bloompg.excitation_threshold",
							 "Re-excite a relation below this fraction of its preceding cardinality.",
							 NULL,
							 &bloompg_excitation_threshold,
							 1.0,
							 0.0,
							 1.0,
							 PGC_USERSET,
							 0,
							 NULL,
								 NULL,
								 NULL);
	DefineCustomRealVariable("bloompg.local_first_fraction",
								 "Evaluate a highly selective sampled local qual before transfer filters.",
								 NULL,
								 &bloompg_local_first_fraction,
								 0.001,
								 0.0,
								 1.0,
								 PGC_USERSET,
								 0,
								 NULL,
								 NULL,
								 NULL);
	DefineCustomIntVariable("bloompg.bits_per_key",
							"Bloom-filter budget per inserted key.",
							NULL,
							&bloompg_bits_per_key,
							24,
							4,
							64,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);
	DefineCustomIntVariable("bloompg.max_filter_bytes",
							"Maximum memory used by one Bloom filter.",
							NULL,
							&bloompg_max_filter_bytes,
							64 * 1024 * 1024,
							512,
							1024 * 1024 * 1024,
							PGC_USERSET,
							GUC_UNIT_BYTE,
							NULL,
								NULL,
								NULL);
	DefineCustomIntVariable("bloompg.materialization_memory",
								"Total memory budget for one query's optimizer-time columnar materializations and parallel DSM copies.",
								NULL,
								&bloompg_materialization_memory_kb,
								materialization_default_kb,
								64,
								INT_MAX,
								PGC_USERSET,
								GUC_UNIT_KB,
								NULL,
								NULL,
								NULL);
	DefineCustomStringVariable("bloompg.sample_mode",
							   "Base-relation sampling mode: prepared or instant.",
							   NULL,
							   &bloompg_sample_mode,
							   "prepared",
							   PGC_USERSET,
							   0,
							   bloompg_check_sample_mode,
							   NULL,
							   NULL);

	bloompg_scan_init();
	bloompg_runtime_init();
	bloompg_sampling_init();
	bloompg_replan_init();
	bloompg_profile_init();
	bloompg_planner_init();
}

void
_PG_fini(void)
{
	bloompg_planner_fini();
	bloompg_profile_fini();
	bloompg_replan_fini();
	bloompg_sampling_fini();
	bloompg_runtime_fini();
}

Datum
bloompg_version(PG_FUNCTION_ARGS)
{
	(void) fcinfo;
	PG_RETURN_TEXT_P(cstring_to_text(BLOOMPG_VERSION_STRING));
}

Datum
bloompg_last_trace(PG_FUNCTION_ARGS)
{
	(void) fcinfo;
	PG_RETURN_TEXT_P(cstring_to_text(bloompg_get_last_trace()));
}

Datum
bloompg_last_profile(PG_FUNCTION_ARGS)
{
	Datum result;

	(void) fcinfo;
	result = DirectFunctionCall1(jsonb_in,
		CStringGetDatum(bloompg_profile_last_json()));
	PG_RETURN_DATUM(result);
}
