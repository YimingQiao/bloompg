#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/profile.h"
#include "bloompg/runtime.h"

#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "optimizer/cost.h"
#include "portability/instr_time.h"
#include "utils/json.h"
#include "utils/memutils.h"

#define BLOOMPG_PROFILE_HISTORY 64
#define BLOOMPG_PROFILE_QUERY_BYTES 2048

typedef struct BloomPGPlannedProfile
{
	PlannedStmt *statement;
	uint64		query_id;
	char	   *query;
	char	   *transfer_trace;
	char	   *transfer_json;
	char	   *p1_path_transform_json;
	double		planning_ms;
	double		p0_ms;
	double		native_preview_ms;
	double		graph_ms;
	double		transfer_ms;
	double		statistics_ms;
	double		p1_ms;
	uint64		p1_cardinality_adjustments;
	double		p1_cardinality_max_ratio;
	int			work_mem_kb;
	double		hash_mem_multiplier;
	int			max_parallel_workers_per_gather;
	int			materialization_memory_kb;
	bool		replanned;
	bool		p1_nestloop_suppressed;
	bool		p1_parallel_biased;
	bool		transfer_failed;
	int		transfer_error_code;
	uint64	   *materialization_tokens;
	int		materialization_count;
} BloomPGPlannedProfile;

typedef struct BloomPGActiveProfile
{
	QueryDesc  *query_desc;
	BloomPGPlannedProfile *planned;
	instr_time	total_started;
	double		start_ms;
	double		run_ms;
	double		finish_ms;
	double		end_ms;
	uint64		run_calls;
	uint64		finish_calls;
	char	   *vector_runtime_json;
} BloomPGActiveProfile;

static ExecutorStart_hook_type previous_executor_start_hook = NULL;
static ExecutorRun_hook_type previous_executor_run_hook = NULL;
static ExecutorFinish_hook_type previous_executor_finish_hook = NULL;
static ExecutorEnd_hook_type previous_executor_end_hook = NULL;
static List *planned_profiles = NIL;
static List *active_profiles = NIL;
static char *last_profile_json = NULL;

static double
bloompg_profile_elapsed_ms(instr_time started)
{
	instr_time elapsed;

	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	return INSTR_TIME_GET_MILLISEC(elapsed);
}

static char *
bloompg_profile_copy_query(const char *query_string)
{
	Size length;
	char *result;

	if (query_string == NULL)
		return pstrdup("");
	length = Min(strlen(query_string), (Size) BLOOMPG_PROFILE_QUERY_BYTES);
	result = palloc(length + 1);
	memcpy(result, query_string, length);
	result[length] = '\0';
	return result;
}

static BloomPGPlannedProfile *
bloompg_profile_find_plan(PlannedStmt *statement)
{
	ListCell *cell;

	foreach(cell, planned_profiles)
	{
		BloomPGPlannedProfile *profile = lfirst(cell);

		if (profile->statement == statement)
			return profile;
	}
	return NULL;
}

static BloomPGActiveProfile *
bloompg_profile_find_active(QueryDesc *query_desc)
{
	ListCell *cell;

	foreach(cell, active_profiles)
	{
		BloomPGActiveProfile *profile = lfirst(cell);

		if (profile->query_desc == query_desc)
			return profile;
	}
	return NULL;
}

static bool
bloompg_profile_plan_is_active(BloomPGPlannedProfile *planned)
{
	ListCell *cell;

	foreach(cell, active_profiles)
	{
		BloomPGActiveProfile *active = lfirst(cell);

		if (active->planned == planned)
			return true;
	}
	return false;
}

static void
bloompg_profile_free_plan(BloomPGPlannedProfile *profile)
{
	if (profile->query != NULL)
		pfree(profile->query);
	if (profile->transfer_trace != NULL)
		pfree(profile->transfer_trace);
	if (profile->transfer_json != NULL)
		pfree(profile->transfer_json);
	if (profile->p1_path_transform_json != NULL)
		pfree(profile->p1_path_transform_json);
	if (profile->materialization_tokens != NULL)
		pfree(profile->materialization_tokens);
	pfree(profile);
}

static void
bloompg_profile_trim_history(void)
{
	while (list_length(planned_profiles) > BLOOMPG_PROFILE_HISTORY)
	{
		BloomPGPlannedProfile *victim = llast(planned_profiles);

		if (bloompg_profile_plan_is_active(victim))
			break;
		planned_profiles = list_delete_ptr(planned_profiles, victim);
		bloompg_profile_free_plan(victim);
	}
}

static char *
bloompg_profile_build_json(BloomPGPlannedProfile *planned,
	BloomPGActiveProfile *active, const char *status, uint64 rows,
	double execution_ms)
{
	StringInfoData json;

	initStringInfo(&json);
	appendStringInfo(&json, "{\"status\":");
	escape_json(&json, status);
	appendStringInfo(&json, ",\"query_id\":" UINT64_FORMAT ",\"query\":",
		planned->query_id);
	escape_json(&json, planned->query);
	appendStringInfo(&json,
		",\"planning_ms\":%.3f,\"execution_ms\":%.3f,\"total_ms\":%.3f",
		planned->planning_ms, execution_ms,
		planned->planning_ms + execution_ms);
	appendStringInfo(&json,
		",\"planning\":{\"total_ms\":%.3f,\"p0_ms\":%.3f,"
		"\"native_preview_ms\":%.3f,"
		"\"graph_ms\":%.3f,\"transfer_ms\":%.3f,"
		"\"statistics_ms\":%.3f,\"p1_ms\":%.3f,"
		"\"p1_cardinality_adjustments\":" UINT64_FORMAT ","
			"\"p1_cardinality_max_ratio\":%.6f,"
			"\"replanned\":%s,\"p1_nestloop_suppressed\":%s,"
			"\"p1_parallel_biased\":%s,"
			"\"transfer_failed\":%s",
		planned->planning_ms, planned->p0_ms, planned->native_preview_ms,
		planned->graph_ms,
		planned->transfer_ms, planned->statistics_ms, planned->p1_ms,
		planned->p1_cardinality_adjustments,
		planned->p1_cardinality_max_ratio,
			planned->replanned ? "true" : "false",
			planned->p1_nestloop_suppressed ? "true" : "false",
			planned->p1_parallel_biased ? "true" : "false",
			planned->transfer_failed ? "true" : "false");
	if (planned->transfer_failed)
		appendStringInfo(&json, ",\"transfer_sqlstate\":\"%s\"",
			unpack_sql_state(planned->transfer_error_code));
	appendStringInfoString(&json, ",\"path_transform\":");
	if (planned->p1_path_transform_json != NULL &&
		planned->p1_path_transform_json[0] == '{')
		appendStringInfoString(&json, planned->p1_path_transform_json);
	else
		appendStringInfoString(&json, "{}");
	appendStringInfoChar(&json, '}');
	appendStringInfo(&json,
		",\"settings\":{\"work_mem_kb\":%d,\"hash_mem_multiplier\":%.3f,"
		"\"hash_memory_limit_kb\":%.0f,"
		"\"max_parallel_workers_per_gather\":%d,"
		"\"materialization_memory_kb\":%d}",
		planned->work_mem_kb, planned->hash_mem_multiplier,
		planned->work_mem_kb * planned->hash_mem_multiplier,
		planned->max_parallel_workers_per_gather,
		planned->materialization_memory_kb);
	if (active != NULL)
	{
		appendStringInfo(&json,
			",\"executor\":{\"start_ms\":%.3f,\"run_ms\":%.3f,"
			"\"finish_ms\":%.3f,\"end_ms\":%.3f,"
			"\"run_calls\":" UINT64_FORMAT ",\"finish_calls\":" UINT64_FORMAT
			",\"rows\":" UINT64_FORMAT,
			active->start_ms, active->run_ms, active->finish_ms, active->end_ms,
			active->run_calls, active->finish_calls, rows);
		if (active->vector_runtime_json != NULL)
		{
			appendStringInfoString(&json, ",\"vectors\":");
			appendStringInfoString(&json, active->vector_runtime_json);
		}
		appendStringInfoChar(&json, '}');
	}
	appendStringInfoString(&json, ",\"transfer_trace\":");
	escape_json(&json, planned->transfer_trace);
	appendStringInfoString(&json, ",\"transfer\":");
	if (planned->transfer_json != NULL && planned->transfer_json[0] == '{')
		appendStringInfoString(&json, planned->transfer_json);
	else
		appendStringInfoString(&json, "null");
	appendStringInfoChar(&json, '}');
	return json.data;
}

static void
bloompg_profile_set_last_json(char *json)
{
	MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);

	if (last_profile_json != NULL)
		pfree(last_profile_json);
	last_profile_json = pstrdup(json != NULL ? json : "{\"status\":\"empty\"}");
	MemoryContextSwitchTo(old_context);
}

static void
bloompg_profile_remove_active(BloomPGActiveProfile *active)
{
	active_profiles = list_delete_ptr(active_profiles, active);
	if (active->vector_runtime_json != NULL)
		pfree(active->vector_runtime_json);
	pfree(active);
}

static char *
bloompg_profile_collect_vector_runtime(BloomPGActiveProfile *active,
	Snapshot snapshot)
{
	StringInfoData json;
	uint64 total_rows = 0;
	uint64 total_live_rows = 0;
	uint64 total_chunks = 0;
	uint64 total_varlen_values = 0;
	uint64 total_selection_rewrites = 0;
	uint64 total_selection_examined = 0;
	uint64 total_selection_rejected = 0;
	uint64 total_slot_reconstructions = 0;
	Size total_memory = 0;
	Size total_varlen_bytes = 0;
	int stores = 0;
	int index;

	initStringInfo(&json);
	appendStringInfoString(&json, "{\"stores\":[");
	for (index = 0; index < active->planned->materialization_count; index++)
	{
		uint64 token = active->planned->materialization_tokens[index];
		BloomPGRuntimeMaterializationEntry *entry =
			bloompg_runtime_find_materialization(token, snapshot);
		BloomPGColumnStore *store;

		if (entry == NULL || entry->store == NULL)
			continue;
		store = entry->store;
		if (stores++ > 0)
			appendStringInfoChar(&json, ',');
		appendStringInfo(&json,
			"{\"token\":" UINT64_FORMAT ",\"format\":\"%s\","
			"\"rows\":" UINT64_FORMAT ",\"live_rows\":" UINT64_FORMAT
			",\"chunks\":%u,\"memory_bytes\":%zu,\"varlen_values\":"
			UINT64_FORMAT ",\"varlen_bytes\":%zu,\"selection_rewrites\":"
			UINT64_FORMAT ",\"selection_rows_examined\":" UINT64_FORMAT
			",\"selection_rows_rejected\":" UINT64_FORMAT
			",\"slot_reconstructions\":" UINT64_FORMAT "}",
			token, bloompg_column_store_spilled(store) ? "spill" : "vector",
			bloompg_column_store_rows(store),
			bloompg_column_store_live_rows(store),
			bloompg_column_store_vector_chunks(store),
			bloompg_column_store_memory_bytes(store),
			bloompg_column_store_varlen_values(store),
			bloompg_column_store_varlen_bytes(store),
			bloompg_column_store_selection_rewrites(store),
			bloompg_column_store_selection_rows_examined(store),
			bloompg_column_store_selection_rows_rejected(store),
			bloompg_column_store_slot_reconstructions(store));
		total_rows += bloompg_column_store_rows(store);
		total_live_rows += bloompg_column_store_live_rows(store);
		total_chunks += bloompg_column_store_vector_chunks(store);
		total_memory = add_size(total_memory,
			bloompg_column_store_memory_bytes(store));
		total_varlen_values += bloompg_column_store_varlen_values(store);
		total_varlen_bytes = add_size(total_varlen_bytes,
			bloompg_column_store_varlen_bytes(store));
		total_selection_rewrites +=
			bloompg_column_store_selection_rewrites(store);
		total_selection_examined +=
			bloompg_column_store_selection_rows_examined(store);
		total_selection_rejected +=
			bloompg_column_store_selection_rows_rejected(store);
		total_slot_reconstructions +=
			bloompg_column_store_slot_reconstructions(store);
	}
	appendStringInfo(&json,
		"],\"totals\":{\"stores\":%d,\"rows\":" UINT64_FORMAT
		",\"live_rows\":" UINT64_FORMAT ",\"chunks\":" UINT64_FORMAT
		",\"memory_bytes\":%zu,\"varlen_values\":" UINT64_FORMAT
		",\"varlen_bytes\":%zu,\"selection_rewrites\":" UINT64_FORMAT
		",\"selection_rows_examined\":" UINT64_FORMAT
		",\"selection_rows_rejected\":" UINT64_FORMAT
		",\"slot_reconstructions\":" UINT64_FORMAT "}}",
		stores, total_rows, total_live_rows, total_chunks, total_memory,
		total_varlen_values, total_varlen_bytes, total_selection_rewrites,
		total_selection_examined, total_selection_rejected,
		total_slot_reconstructions);
	return json.data;
}

static void
bloompg_profile_call_start(QueryDesc *query_desc, int eflags)
{
	if (previous_executor_start_hook != NULL)
		previous_executor_start_hook(query_desc, eflags);
	else
		standard_ExecutorStart(query_desc, eflags);
}

static void
bloompg_profile_call_run(QueryDesc *query_desc, ScanDirection direction,
	uint64 count)
{
	if (previous_executor_run_hook != NULL)
		previous_executor_run_hook(query_desc, direction, count);
	else
		standard_ExecutorRun(query_desc, direction, count);
}

static void
bloompg_profile_call_finish(QueryDesc *query_desc)
{
	if (previous_executor_finish_hook != NULL)
		previous_executor_finish_hook(query_desc);
	else
		standard_ExecutorFinish(query_desc);
}

static void
bloompg_profile_call_end(QueryDesc *query_desc)
{
	if (previous_executor_end_hook != NULL)
		previous_executor_end_hook(query_desc);
	else
		standard_ExecutorEnd(query_desc);
}

static void
bloompg_profile_executor_start(QueryDesc *query_desc, int eflags)
{
	BloomPGPlannedProfile *planned = NULL;
	BloomPGActiveProfile *active = NULL;
	instr_time started;

	if (bloompg_profile_enabled && query_desc->plannedstmt != NULL)
		planned = bloompg_profile_find_plan(query_desc->plannedstmt);
	if (planned != NULL)
	{
		MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);

		active = palloc0(sizeof(BloomPGActiveProfile));
		active->query_desc = query_desc;
		active->planned = planned;
		INSTR_TIME_SET_CURRENT(active->total_started);
		active_profiles = lappend(active_profiles, active);
		MemoryContextSwitchTo(old_context);
		INSTR_TIME_SET_CURRENT(started);
	}
	PG_TRY();
	{
		bloompg_profile_call_start(query_desc, eflags);
		if (active != NULL)
			active->start_ms += bloompg_profile_elapsed_ms(started);
	}
	PG_CATCH();
	{
		if (active != NULL)
			bloompg_profile_remove_active(active);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
bloompg_profile_executor_run(QueryDesc *query_desc, ScanDirection direction,
	uint64 count)
{
	BloomPGActiveProfile *active = bloompg_profile_find_active(query_desc);
	instr_time started;

	if (active != NULL)
		INSTR_TIME_SET_CURRENT(started);
	bloompg_profile_call_run(query_desc, direction, count);
	if (active != NULL)
	{
		active->run_ms += bloompg_profile_elapsed_ms(started);
		active->run_calls++;
	}
}

static void
bloompg_profile_executor_finish(QueryDesc *query_desc)
{
	BloomPGActiveProfile *active = bloompg_profile_find_active(query_desc);
	instr_time started;

	if (active != NULL)
		INSTR_TIME_SET_CURRENT(started);
	bloompg_profile_call_finish(query_desc);
	if (active != NULL)
	{
		active->finish_ms += bloompg_profile_elapsed_ms(started);
		active->finish_calls++;
	}
}

static void
bloompg_profile_executor_end(QueryDesc *query_desc)
{
	BloomPGActiveProfile *active = bloompg_profile_find_active(query_desc);
	uint64 rows = query_desc->estate != NULL ? query_desc->estate->es_processed : 0;
	instr_time started;
	double execution_ms;
	char *json;

	if (active == NULL)
	{
		bloompg_profile_call_end(query_desc);
		return;
	}
	INSTR_TIME_SET_CURRENT(started);
	{
		MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);

		active->vector_runtime_json = bloompg_profile_collect_vector_runtime(
			active, query_desc->estate != NULL ? query_desc->estate->es_snapshot : NULL);
		MemoryContextSwitchTo(old_context);
	}
	bloompg_profile_call_end(query_desc);
	active->end_ms += bloompg_profile_elapsed_ms(started);
	execution_ms = bloompg_profile_elapsed_ms(active->total_started);
	json = bloompg_profile_build_json(active->planned, active, "complete", rows,
		execution_ms);
	bloompg_profile_set_last_json(json);
	if (bloompg_profile_log)
		elog(LOG, "bloompg profile %s", json);
	pfree(json);
	bloompg_profile_remove_active(active);
}

void
bloompg_profile_remember_plan(PlannedStmt *statement,
	const char *query_string, const BloomPGPlanProfileInput *input)
{
	BloomPGPlannedProfile *profile;
	MemoryContext old_context;
	char *json;

	if (!bloompg_profile_enabled || statement == NULL || input == NULL)
		return;
	old_context = MemoryContextSwitchTo(TopMemoryContext);
	profile = bloompg_profile_find_plan(statement);
	if (profile == NULL)
	{
		profile = palloc0(sizeof(BloomPGPlannedProfile));
		profile->statement = statement;
		planned_profiles = lcons(profile, planned_profiles);
	}
	else
	{
		if (profile->query != NULL)
			pfree(profile->query);
		if (profile->transfer_trace != NULL)
			pfree(profile->transfer_trace);
		if (profile->transfer_json != NULL)
			pfree(profile->transfer_json);
		if (profile->p1_path_transform_json != NULL)
			pfree(profile->p1_path_transform_json);
		if (profile->materialization_tokens != NULL)
			pfree(profile->materialization_tokens);
	}
	profile->query_id = statement->queryId;
	profile->query = bloompg_profile_copy_query(query_string);
	profile->transfer_trace = pstrdup(input->transfer_trace != NULL ?
		input->transfer_trace : "");
	profile->transfer_json = input->transfer_json != NULL ?
		pstrdup(input->transfer_json) : NULL;
	profile->p1_path_transform_json =
		input->p1_path_transform_json != NULL ?
		pstrdup(input->p1_path_transform_json) : NULL;
	profile->planning_ms = input->planning_ms;
	profile->p0_ms = input->p0_ms;
	profile->native_preview_ms = input->native_preview_ms;
	profile->graph_ms = input->graph_ms;
	profile->transfer_ms = input->transfer_ms;
	profile->statistics_ms = input->statistics_ms;
	profile->p1_ms = input->p1_ms;
	profile->p1_cardinality_adjustments =
		input->p1_cardinality_adjustments;
	profile->p1_cardinality_max_ratio =
		input->p1_cardinality_max_ratio;
	profile->work_mem_kb = work_mem;
	profile->hash_mem_multiplier = hash_mem_multiplier;
	profile->max_parallel_workers_per_gather =
		max_parallel_workers_per_gather;
	profile->materialization_memory_kb =
		bloompg_materialization_memory_kb;
	profile->replanned = input->replanned;
	profile->p1_nestloop_suppressed = input->p1_nestloop_suppressed;
	profile->p1_parallel_biased = input->p1_parallel_biased;
	profile->transfer_failed = input->transfer_failed;
	profile->transfer_error_code = input->transfer_error_code;
	profile->materialization_count = list_length(input->materialization_tokens);
	profile->materialization_tokens = NULL;
	if (profile->materialization_count > 0)
	{
		ListCell *cell;
		int index = 0;

		profile->materialization_tokens =
			palloc_array(uint64, profile->materialization_count);
		foreach(cell, input->materialization_tokens)
			profile->materialization_tokens[index++] =
				*((uint64 *) lfirst(cell));
	}
	bloompg_profile_trim_history();
	json = bloompg_profile_build_json(profile, NULL, "planned", 0, 0.0);
	bloompg_profile_set_last_json(json);
	pfree(json);
	MemoryContextSwitchTo(old_context);
}

const char *
bloompg_profile_last_json(void)
{
	return last_profile_json != NULL ? last_profile_json : "{\"status\":\"empty\"}";
}

void
bloompg_profile_init(void)
{
	MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);

	last_profile_json = pstrdup("{\"status\":\"empty\"}");
	MemoryContextSwitchTo(old_context);
	previous_executor_start_hook = ExecutorStart_hook;
	ExecutorStart_hook = bloompg_profile_executor_start;
	previous_executor_run_hook = ExecutorRun_hook;
	ExecutorRun_hook = bloompg_profile_executor_run;
	previous_executor_finish_hook = ExecutorFinish_hook;
	ExecutorFinish_hook = bloompg_profile_executor_finish;
	previous_executor_end_hook = ExecutorEnd_hook;
	ExecutorEnd_hook = bloompg_profile_executor_end;
}

void
bloompg_profile_fini(void)
{
	ListCell *cell;

	if (ExecutorEnd_hook == bloompg_profile_executor_end)
		ExecutorEnd_hook = previous_executor_end_hook;
	if (ExecutorFinish_hook == bloompg_profile_executor_finish)
		ExecutorFinish_hook = previous_executor_finish_hook;
	if (ExecutorRun_hook == bloompg_profile_executor_run)
		ExecutorRun_hook = previous_executor_run_hook;
	if (ExecutorStart_hook == bloompg_profile_executor_start)
		ExecutorStart_hook = previous_executor_start_hook;
	foreach(cell, active_profiles)
		pfree(lfirst(cell));
	list_free(active_profiles);
	active_profiles = NIL;
	foreach(cell, planned_profiles)
		bloompg_profile_free_plan(lfirst(cell));
	list_free(planned_profiles);
	planned_profiles = NIL;
	if (last_profile_json != NULL)
	{
		pfree(last_profile_json);
		last_profile_json = NULL;
	}
}
