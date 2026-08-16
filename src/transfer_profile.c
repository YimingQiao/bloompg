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
#include "utils/date.h"
#include "utils/typcache.h"

#include <float.h>

/* Transfer trace and structured profiling serialization. */

void
bloompg_transfer_profile_append_step(StringInfo json, int step,
	BloomPGRelationState *source, BloomPGPendingTransfer *pending,
	uint64 rows_seen, uint64 materialization_token)
{
	int key;

	if (json->len > 0)
		appendStringInfoChar(json, ',');
	appendStringInfo(json,
		"{\"step\":%d,\"source_rti\":%u,\"source\":", step,
		source->rti);
	escape_json(json, source->table->alias != NULL ? source->table->alias : "?");
	appendStringInfo(json,
		",\"selection_join_index_sources\":%d,"
		"\"selection_index_ready\":%s,\"selection_guarded\":%s,"
		"\"selection_guard_fallback\":%s",
		bms_num_members(source->join_index_sources),
		source->selection_index_ready ? "true" : "false",
		source->selection_guarded ? "true" : "false",
		source->selection_guard_fallback ? "true" : "false");
	appendStringInfo(json, ",\"destination_rti\":%u,\"destination\":",
		pending->destination->rti);
	escape_json(json, pending->destination->table->alias != NULL ?
		pending->destination->table->alias : "?");
	appendStringInfo(json,
		",\"rows\":" UINT64_FORMAT ",\"filter_token\":" UINT64_FORMAT
		",\"materialization_token\":" UINT64_FORMAT ",\"filter_kind\":",
		rows_seen, pending->spec.token, materialization_token);
	escape_json(json, pending->filter->kind == BLOOMPG_FILTER_BITMAP ?
		"bitmap" : "bloom");
	appendStringInfoString(json, ",\"filter_cache\":");
	escape_json(json, pending->build_filter ? "miss" : "hit");
	appendStringInfo(json, ",\"equality_domain_version\":" UINT64_FORMAT,
		pending->equality_domain_version);
	appendStringInfoString(json, ",\"keys\":[");
	for (key = 0; key < pending->spec.nkeys; key++)
	{
		if (key > 0)
			appendStringInfoChar(json, ',');
		appendStringInfo(json,
			"{\"operator_oid\":%u,\"build_slot_attno\":%d,"
			"\"probe_slot_attno\":%d,\"probe_base_attno\":%d}",
			pending->spec.operators[key], pending->spec.build_attributes[key],
			pending->spec.probe_attributes[key],
			pending->spec.probe_base_attributes[key]);
	}
	appendStringInfoString(json, "]}");
}

char *
bloompg_transfer_finish_profile(BloomPGRelationState **states,
	int relation_count, BloomPGTransferOutcome outcome,
	StringInfoData *detail_target, StringInfoData *profile_steps_target)
{
	StringInfoData detail = *detail_target;
	StringInfoData profile_steps;
	StringInfoData profile_relations;
	bool collect_profile = profile_steps_target != NULL;
	uint64 profile_parallel_scans = 0;
	uint64 profile_parallel_workers_requested = 0;
	uint64 profile_parallel_workers_launched = 0;
	uint64 profile_parallel_rows = 0;
	double profile_parallel_ms = 0.0;
	uint64 profile_parallel_waves = 0;
	uint64 profile_parallel_append_waves = 0;
	uint64 profile_parallel_wave_relations = 0;
	uint64 profile_one_pass_bitmap_builds = 0;
	uint64 profile_filter_finalize_rescans = 0;
	uint64 profile_filter_finalize_rows = 0;
	uint64 profile_precomputed_key_sets = 0;
	uint64 profile_scanned_key_sets = 0;
	uint64 profile_index_probe_scans = 0;
	uint64 profile_index_probe_batches = 0;
	uint64 profile_index_probe_keys = 0;
	uint64 profile_index_probe_rows = 0;
	double profile_index_probe_ms = 0.0;

	if (collect_profile)
	{
		profile_steps = *profile_steps_target;
		initStringInfo(&profile_relations);
	}
	for (int rti = 1; rti <= relation_count; rti++)
	{
		BloomPGRelationState *state = states[rti];
		BloomPGRuntimeMaterializationEntry *entry = NULL;
		uint64 visible_rows = 0;
		uint64 sample_local_rows = 0;
		uint64 probe_count = 0;
		uint64 probe_rows_examined = 0;
		uint64 probe_filter_checks = 0;
		uint64 probe_filters_applied = 0;
		uint64 probe_cache_hits = 0;
		double load_ms = 0.0;
		double hydrate_ms = 0.0;
		double qual_ms = 0.0;
		double probe_ms = 0.0;
		bool tuple_cache_hit = false;

		if (state == NULL)
			continue;
		if (state->materialization_token != 0)
			entry = bloompg_runtime_find_materialization(
				state->materialization_token, GetActiveSnapshot());
		if (state->sampling_profile.acquisitions > 0)
			appendStringInfo(&detail,
				" sampleio=%u:" UINT64_FORMAT
				" mem=" UINT64_FORMAT " disk=" UINT64_FORMAT
				" build=" UINT64_FORMAT " acquire=%.3fms build_ms=%.3fms",
				state->rti, state->sampling_profile.acquisitions,
				state->sampling_profile.memory_hits,
				state->sampling_profile.disk_hits,
				state->sampling_profile.builds,
				state->sampling_profile.acquire_ms,
				state->sampling_profile.build_ms);
		if (state->exact_scans > 0)
		{
				appendStringInfo(&detail,
					" exact=%u:" UINT64_FORMAT ":%.3fms"
					" base=" UINT64_FORMAT ":%.3fms compact=" UINT64_FORMAT
					":%.3fms kernel=%s filter_build=" UINT64_FORMAT "/"
					UINT64_FORMAT
					":%.3fms prepare=%.3fms finalize=%.3fms"
					" bitmap_onepass=" UINT64_FORMAT " finalize_rescan="
					UINT64_FORMAT ":" UINT64_FORMAT
					" columns=%d/%d access=%s", state->rti,
				state->exact_scans,
				state->exact_scan_ms, state->base_materializations,
				state->base_materialize_ms, state->compactions,
				state->compact_ms,
				state->slot_compactions > 0 ? "slot" : "column",
					state->vector_filter_builds, state->slot_filter_builds,
					state->filter_build_ms,
					state->filter_prepare_ms, state->filter_finalize_ms,
					state->one_pass_bitmap_builds,
					state->filter_finalize_rescans,
					state->filter_finalize_rows,
					state->materialized_output_columns,
					state->original_output_columns,
					state->materialization_access_path != NULL ?
						state->materialization_access_path : "unknown");
				if (state->parallel_scans > 0)
					appendStringInfo(&detail,
						" parallel=" UINT64_FORMAT ":" UINT64_FORMAT "/"
					UINT64_FORMAT " rows=" UINT64_FORMAT ":%.3fms",
					state->parallel_scans,
						state->parallel_workers_launched,
						state->parallel_workers_requested,
						state->parallel_rows, state->parallel_scan_ms);
				if (state->index_probe_scans > 0)
				{
					appendStringInfo(&detail,
						" index_probe=" UINT64_FORMAT ":%u:%d batches="
						UINT64_FORMAT " keys="
						UINT64_FORMAT " rows=" UINT64_FORMAT ":%.3fms"
						" fraction=%.6f token=" UINT64_FORMAT
						" candidates=" UINT64_FORMAT " sample=" UINT64_FORMAT
						"/" UINT64_FORMAT ":%.3fms source=%s",
						state->index_probe_scans, state->index_probe_oid,
						state->index_probe_attribute, state->index_probe_batches,
						state->index_probe_keys,
						state->index_probe_rows, state->index_probe_ms,
						state->index_probe_fraction,
						state->index_probe_filter_token,
						state->index_probe_candidates,
						state->index_probe_sample_matches,
						state->index_probe_sample_rows,
						state->index_probe_estimate_ms,
						state->index_probe_estimate_source != NULL ?
							state->index_probe_estimate_source : "none");
					if (state->index_bitmap_scans > 0)
					{
						appendStringInfo(&detail,
							" bitmap=" UINT64_FORMAT " sources=" UINT64_FORMAT
							" index_tuples=" UINT64_FORMAT " pages="
							UINT64_FORMAT "/" UINT64_FORMAT " dominated="
							UINT64_FORMAT,
							state->index_bitmap_scans,
							state->index_bitmap_sources,
							state->index_bitmap_index_tuples,
							state->index_bitmap_exact_pages,
							state->index_bitmap_lossy_pages,
							state->index_probe_dominated_candidates);
						for (int input_index = 0;
							input_index < state->index_bitmap_input_count;
							input_index++)
						{
							BloomPGIndexBitmapInputProfile *input =
								&state->index_bitmap_inputs[input_index];

							appendStringInfo(&detail,
								" bitmap_input=%u:%d:" UINT64_FORMAT ":"
								UINT64_FORMAT ":" UINT64_FORMAT ":%.6f",
								input->index_oid, input->attribute,
								input->filter_token, input->keys,
								input->index_tuples,
								input->estimated_fraction);
						}
					}
				}
				if (state->access_plan_attempts > 0)
					appendStringInfo(&detail,
						" access_plan=" UINT64_FORMAT ":%s exact="
						UINT64_FORMAT "/" UINT64_FORMAT
						" cost=%.2f..%.2f rows=%.0f plan=%.3fms"
						" nodes=" UINT64_FORMAT "/" UINT64_FORMAT "/"
						UINT64_FORMAT "/" UINT64_FORMAT "/" UINT64_FORMAT
						" reject=%s",
						state->access_plan_attempts,
						state->access_plan_kind != NULL ?
							state->access_plan_kind : "none",
						state->access_plan_exact_filters,
						state->access_plan_exact_keys,
						state->access_plan_startup_cost,
						state->access_plan_total_cost,
						state->access_plan_rows, state->access_plan_ms,
						state->access_plan_seq_scans,
						state->access_plan_index_scans,
						state->access_plan_index_only_scans,
						state->access_plan_bitmap_heap_scans,
						state->access_plan_bitmap_index_scans,
						state->access_plan_rejection != NULL ?
							state->access_plan_rejection : "none");
				if (state->parallel_wave_id != 0)
					appendStringInfo(&detail,
						" wave=" UINT64_FORMAT ":%s:%s:%s peer=%u workers="
						UINT64_FORMAT "/" UINT64_FORMAT ":%.3fms",
						state->parallel_wave_id, state->parallel_wave_kind,
						state->parallel_wave_layout,
						state->parallel_wave_coordinator ? "coordinator" : "peer",
						state->parallel_wave_peer_rti,
						state->parallel_wave_workers_launched,
						state->parallel_wave_workers_requested,
						state->parallel_wave_ms);
			if (entry != NULL && entry->store != NULL)
				appendStringInfo(&detail,
					" store=%s:" UINT64_FORMAT "/" UINT64_FORMAT
					":%zuB peak=%zuB limit=%zuB spill_at=" UINT64_FORMAT
					" chunks=%u varlen=" UINT64_FORMAT ":%zuB"
					" selection=" UINT64_FORMAT "/" UINT64_FORMAT "/"
					UINT64_FORMAT " slots=" UINT64_FORMAT,
					bloompg_column_store_spilled(entry->store) ? "spill" : "vector",
					bloompg_column_store_live_rows(entry->store),
					bloompg_column_store_rows(entry->store),
					bloompg_column_store_memory_bytes(entry->store),
					bloompg_column_store_peak_memory_bytes(entry->store),
					bloompg_column_store_memory_limit(entry->store),
					bloompg_column_store_spill_rows(entry->store),
					bloompg_column_store_vector_chunks(entry->store),
					bloompg_column_store_varlen_values(entry->store),
					bloompg_column_store_varlen_bytes(entry->store),
					bloompg_column_store_selection_rewrites(entry->store),
					bloompg_column_store_selection_rows_examined(entry->store),
					bloompg_column_store_selection_rows_rejected(entry->store),
					bloompg_column_store_slot_reconstructions(entry->store));
			if (state->prefilter_scans > 0)
				appendStringInfo(&detail,
					" prefilter=" UINT64_FORMAT ":" UINT64_FORMAT "/"
					UINT64_FORMAT " local_reject=" UINT64_FORMAT
					" order=%s local_est=%.6f transfer_est=%.6f"
					" order_cost=%s:%.3f/%.3fms",
					state->prefilter_scans, state->prefilter_rejected,
					state->prefilter_rows, state->local_rejected,
					state->local_first_scans > 0 ? "local-first" : "filter-first",
					state->prefilter_local_fraction,
					state->prefilter_transfer_fraction,
					state->prefilter_costed ? "sample" : "fallback",
					state->prefilter_local_first_score,
					state->prefilter_filter_first_score);
		}
		if (state->sample_workspace != NULL)
		{
			bloompg_sample_workspace_stats(state->sample_workspace,
				&visible_rows, &sample_local_rows, &load_ms, &hydrate_ms,
				&qual_ms, &tuple_cache_hit, &probe_count, &probe_ms,
				&probe_rows_examined, &probe_filter_checks,
				&probe_filters_applied, &probe_cache_hits);
			appendStringInfo(&detail,
				" samplemem=%u:" UINT64_FORMAT "/" UINT64_FORMAT
				" load=%.3fms hydrate=%.3fms qual=%.3fms cache=%s"
				" probes=" UINT64_FORMAT ":%.3fms rows=" UINT64_FORMAT
				" checks=" UINT64_FORMAT " filters=" UINT64_FORMAT
				" hits=" UINT64_FORMAT,
				state->rti, sample_local_rows, visible_rows, load_ms, hydrate_ms,
				qual_ms, tuple_cache_hit ? "hit" : "miss", probe_count,
				probe_ms, probe_rows_examined, probe_filter_checks,
				probe_filters_applied, probe_cache_hits);
		}
		if (state->intermediate_sampling_profile.calls > 0)
			appendStringInfo(&detail,
				" intermediate_sample=%u:" UINT64_FORMAT "/" UINT64_FORMAT
				" fraction=%.6f calls=" UINT64_FORMAT " probe=%.3fms source=%s",
				state->rti,
				state->intermediate_sampling_profile.rows_passed,
				state->intermediate_sampling_profile.rows_examined,
				state->intermediate_sampling_profile.last_fraction,
				state->intermediate_sampling_profile.calls,
				state->intermediate_sampling_profile.probe_ms,
				state->intermediate_sampling_profile.vectorized ?
					"vector" : "spill");
		if (collect_profile)
		{
			profile_one_pass_bitmap_builds +=
				state->one_pass_bitmap_builds;
			profile_filter_finalize_rescans +=
				state->filter_finalize_rescans;
			profile_filter_finalize_rows += state->filter_finalize_rows;
			if (state->statistics != NULL)
			{
				profile_precomputed_key_sets +=
					state->statistics->precomputed_key_sets;
				profile_scanned_key_sets +=
					state->statistics->scanned_key_sets;
			}
			profile_parallel_scans += state->parallel_scans;
			profile_index_probe_scans += state->index_probe_scans;
			profile_index_probe_batches += state->index_probe_batches;
			profile_index_probe_keys += state->index_probe_keys;
			profile_index_probe_rows += state->index_probe_rows;
			profile_index_probe_ms += state->index_probe_ms;
			profile_parallel_workers_requested +=
				state->parallel_workers_requested;
				profile_parallel_workers_launched +=
					state->parallel_workers_launched;
				profile_parallel_rows += state->parallel_rows;
				if (state->parallel_wave_id == 0)
				{
					profile_parallel_waves += state->parallel_scans;
					profile_parallel_ms += state->parallel_scan_ms;
				}
				else
				{
					profile_parallel_wave_relations++;
					if (state->parallel_wave_coordinator)
					{
						profile_parallel_waves++;
						profile_parallel_ms += state->parallel_wave_ms;
						if (strcmp(state->parallel_wave_kind,
								"parallel-append") == 0)
							profile_parallel_append_waves++;
				}
				}
			if (profile_relations.len > 0)
				appendStringInfoChar(&profile_relations, ',');
			appendStringInfo(&profile_relations, "{\"rti\":%u,\"alias\":",
				state->rti);
			escape_json(&profile_relations,
				state->table->alias != NULL ? state->table->alias : "?");
			appendStringInfo(&profile_relations,
				",\"relation_oid\":%u,\"base_rows\":%.0f,"
				"\"initial_rows\":%.0f,\"final_rows\":%.0f,"
				"\"projection\":{\"columns\":%d,\"original_columns\":%d,"
				"\"attributes\":[",
				state->table->relation_oid, state->base_rows, state->local_rows,
				state->current_rows, state->materialized_output_columns,
				state->original_output_columns);
			bloompg_profile_append_projection_attributes(&profile_relations,
				state);
			appendStringInfoString(&profile_relations,
				"]},\"replacement\":");
			escape_json(&profile_relations, state->replacement_kind);
			appendStringInfo(&profile_relations,
				",\"sampling\":{\"acquisitions\":" UINT64_FORMAT
				",\"memory_hits\":" UINT64_FORMAT
				",\"disk_hits\":" UINT64_FORMAT
				",\"builds\":" UINT64_FORMAT
				",\"acquire_ms\":%.3f,\"build_ms\":%.3f,"
				"\"visible_rows\":" UINT64_FORMAT
				",\"local_rows\":" UINT64_FORMAT
				",\"load_ms\":%.3f,\"hydrate_ms\":%.3f,"
				"\"qual_ms\":%.3f,\"tuple_cache_hit\":%s,"
					"\"probe_calls\":" UINT64_FORMAT ",\"probe_ms\":%.3f,"
					"\"probe_rows_examined\":" UINT64_FORMAT
					",\"probe_filter_checks\":" UINT64_FORMAT
					",\"probe_filters_applied\":" UINT64_FORMAT
					",\"probe_cache_hits\":" UINT64_FORMAT "},",
				state->sampling_profile.acquisitions,
				state->sampling_profile.memory_hits,
				state->sampling_profile.disk_hits,
				state->sampling_profile.builds,
				state->sampling_profile.acquire_ms,
				state->sampling_profile.build_ms, visible_rows,
				sample_local_rows, load_ms, hydrate_ms, qual_ms,
					tuple_cache_hit ? "true" : "false", probe_count, probe_ms,
					probe_rows_examined, probe_filter_checks,
					probe_filters_applied, probe_cache_hits);
			appendStringInfo(&profile_relations,
				"\"intermediate_sampling\":{\"calls\":" UINT64_FORMAT
				",\"source\":\"%s\",\"rows_examined\":" UINT64_FORMAT
				",\"rows_passed\":" UINT64_FORMAT
				",\"last_fraction\":%.6f,\"probe_ms\":%.3f},",
				state->intermediate_sampling_profile.calls,
				state->intermediate_sampling_profile.calls == 0 ? "none" :
					(state->intermediate_sampling_profile.vectorized ?
						"vector" : "spill"),
				state->intermediate_sampling_profile.rows_examined,
				state->intermediate_sampling_profile.rows_passed,
				state->intermediate_sampling_profile.last_fraction,
				state->intermediate_sampling_profile.probe_ms);
			appendStringInfo(&profile_relations,
					"\"materialization\":{\"access_path\":\"%s\","
				"\"selection\":{\"policy\":\"index-wait-guard-v1\","
				"\"join_index_sources\":%d,\"index_ready\":%s,"
				"\"guarded\":%s,\"fallback\":%s},"
				"\"access_plan\":{\"attempts\":" UINT64_FORMAT
				",\"planning_ms\":%.3f,\"kind\":\"%s\","
				"\"rejection\":\"%s\",\"exact_filters\":" UINT64_FORMAT
				",\"exact_keys\":" UINT64_FORMAT
				",\"startup_cost\":%.6f,\"total_cost\":%.6f,"
				"\"rows\":%.3f,\"seq_scans\":" UINT64_FORMAT
				",\"index_scans\":" UINT64_FORMAT
				",\"index_only_scans\":" UINT64_FORMAT
				",\"bitmap_heap_scans\":" UINT64_FORMAT
				",\"bitmap_index_scans\":" UINT64_FORMAT
				",\"gathers\":" UINT64_FORMAT "},"
				"\"exact_scans\":" UINT64_FORMAT
				",\"exact_ms\":%.3f,\"base_scans\":" UINT64_FORMAT
				",\"base_ms\":%.3f,\"compact_scans\":" UINT64_FORMAT
				",\"compact_ms\":%.3f,\"column_compactions\":" UINT64_FORMAT
				",\"slot_compactions\":" UINT64_FORMAT
					",\"vector_filter_builds\":" UINT64_FORMAT
					",\"slot_filter_builds\":" UINT64_FORMAT
					",\"filter_prepare_ms\":%.3f,\"filter_finalize_ms\":%.3f,"
					"\"filter_build_ms\":%.3f,\"one_pass_bitmap_builds\":"
					UINT64_FORMAT ",\"filter_finalize_rescans\":" UINT64_FORMAT
					",\"filter_finalize_rows\":" UINT64_FORMAT
					",\"index_probe\":{\"scans\":" UINT64_FORMAT
					",\"index_oid\":%u,\"attribute\":%d,\"batches\":" UINT64_FORMAT
					",\"keys\":" UINT64_FORMAT
					",\"rows\":" UINT64_FORMAT ",\"scan_ms\":%.3f,"
					"\"estimated_fraction\":%.6f,\"filter_token\":"
					UINT64_FORMAT ",\"candidates\":" UINT64_FORMAT
					",\"estimate_source\":\"%s\",\"sample_rows\":"
					UINT64_FORMAT ",\"sample_matches\":" UINT64_FORMAT
					",\"estimate_ms\":%.3f,\"bitmap_scans\":" UINT64_FORMAT
					",\"bitmap_sources\":" UINT64_FORMAT
					",\"bitmap_index_tuples\":" UINT64_FORMAT
					",\"bitmap_exact_pages\":" UINT64_FORMAT
					",\"bitmap_lossy_pages\":" UINT64_FORMAT
					",\"dominated_candidates\":" UINT64_FORMAT
						"},\"parallel\":{"
				"\"scans\":" UINT64_FORMAT
				",\"workers_requested\":" UINT64_FORMAT
				",\"workers_launched\":" UINT64_FORMAT
					",\"rows\":" UINT64_FORMAT ",\"scan_ms\":%.3f},"
					"\"wave\":",
				state->materialization_access_path != NULL ?
					state->materialization_access_path : "none",
				bms_num_members(state->join_index_sources),
				state->selection_index_ready ? "true" : "false",
				state->selection_guarded ? "true" : "false",
				state->selection_guard_fallback ? "true" : "false",
				state->access_plan_attempts, state->access_plan_ms,
				state->access_plan_kind != NULL ?
					state->access_plan_kind : "none",
				state->access_plan_rejection != NULL ?
					state->access_plan_rejection : "none",
				state->access_plan_exact_filters,
				state->access_plan_exact_keys,
				state->access_plan_startup_cost,
				state->access_plan_total_cost,
				state->access_plan_rows,
				state->access_plan_seq_scans,
				state->access_plan_index_scans,
				state->access_plan_index_only_scans,
				state->access_plan_bitmap_heap_scans,
				state->access_plan_bitmap_index_scans,
				state->access_plan_gathers,
				state->exact_scans, state->exact_scan_ms,
				state->base_materializations, state->base_materialize_ms,
				state->compactions, state->compact_ms,
					state->column_compactions, state->slot_compactions,
					state->vector_filter_builds, state->slot_filter_builds,
					state->filter_prepare_ms, state->filter_finalize_ms,
					state->filter_build_ms, state->one_pass_bitmap_builds,
					state->filter_finalize_rescans,
					state->filter_finalize_rows, state->index_probe_scans,
					state->index_probe_oid, state->index_probe_attribute,
					state->index_probe_batches, state->index_probe_keys,
					state->index_probe_rows,
					state->index_probe_ms, state->index_probe_fraction,
					state->index_probe_filter_token,
					state->index_probe_candidates,
					state->index_probe_estimate_source != NULL ?
						state->index_probe_estimate_source : "none",
					state->index_probe_sample_rows,
					state->index_probe_sample_matches,
					state->index_probe_estimate_ms,
					state->index_bitmap_scans,
					state->index_bitmap_sources,
					state->index_bitmap_index_tuples,
					state->index_bitmap_exact_pages,
					state->index_bitmap_lossy_pages,
					state->index_probe_dominated_candidates,
					state->parallel_scans,
				state->parallel_workers_requested,
					state->parallel_workers_launched, state->parallel_rows,
					state->parallel_scan_ms);
				if (state->parallel_wave_id != 0)
					appendStringInfo(&profile_relations,
						"{\"id\":" UINT64_FORMAT ",\"kind\":\"%s\","
						"\"layout\":\"%s\","
						"\"role\":\"%s\",\"peer_rti\":%u,"
						"\"workers_requested\":" UINT64_FORMAT
						",\"workers_launched\":" UINT64_FORMAT
						",\"wall_ms\":%.3f}",
						state->parallel_wave_id, state->parallel_wave_kind,
						state->parallel_wave_layout,
						state->parallel_wave_coordinator ? "coordinator" : "peer",
						state->parallel_wave_peer_rti,
						state->parallel_wave_workers_requested,
						state->parallel_wave_workers_launched,
						state->parallel_wave_ms);
				else
					appendStringInfoString(&profile_relations, "null");
			appendStringInfoString(&profile_relations,
				",\"bitmap_inputs\":[");
			for (int input_index = 0;
				input_index < state->index_bitmap_input_count; input_index++)
			{
				BloomPGIndexBitmapInputProfile *input =
					&state->index_bitmap_inputs[input_index];

				if (input_index > 0)
					appendStringInfoChar(&profile_relations, ',');
				appendStringInfo(&profile_relations,
					"{\"index_oid\":%u,\"attribute\":%d,\"filter_token\":"
					UINT64_FORMAT ",\"keys\":" UINT64_FORMAT
					",\"estimated_fraction\":%.6f,\"index_tuples\":"
					UINT64_FORMAT "}",
					input->index_oid, input->attribute, input->filter_token,
					input->keys, input->estimated_fraction,
					input->index_tuples);
			}
			appendStringInfoChar(&profile_relations, ']');
			if (entry != NULL && entry->store != NULL)
				appendStringInfo(&profile_relations,
					",\"store\":{\"format\":\"%s\",\"live_rows\":"
					UINT64_FORMAT ",\"rows\":" UINT64_FORMAT
					",\"memory_bytes\":%zu,\"peak_memory_bytes\":%zu,"
					"\"memory_limit_bytes\":%zu,\"spill_at_rows\":"
					UINT64_FORMAT ",\"vector_size\":%u,\"chunks\":%u,"
					"\"varlen_values\":" UINT64_FORMAT
					",\"varlen_bytes\":%zu,\"selection_rewrites\":"
					UINT64_FORMAT ",\"selection_rows_examined\":" UINT64_FORMAT
					",\"selection_rows_rejected\":" UINT64_FORMAT
					",\"slot_reconstructions\":" UINT64_FORMAT "}",
					bloompg_column_store_spilled(entry->store) ? "spill" : "vector",
					bloompg_column_store_live_rows(entry->store),
					bloompg_column_store_rows(entry->store),
					bloompg_column_store_memory_bytes(entry->store),
					bloompg_column_store_peak_memory_bytes(entry->store),
					bloompg_column_store_memory_limit(entry->store),
					bloompg_column_store_spill_rows(entry->store),
					BLOOMPG_VECTOR_SIZE,
					bloompg_column_store_vector_chunks(entry->store),
					bloompg_column_store_varlen_values(entry->store),
					bloompg_column_store_varlen_bytes(entry->store),
					bloompg_column_store_selection_rewrites(entry->store),
					bloompg_column_store_selection_rows_examined(entry->store),
					bloompg_column_store_selection_rows_rejected(entry->store),
					bloompg_column_store_slot_reconstructions(entry->store));
			else
				appendStringInfoString(&profile_relations, ",\"store\":null");
			appendStringInfo(&profile_relations,
				"},\"prefilter\":{\"scans\":" UINT64_FORMAT
				",\"rows\":" UINT64_FORMAT ",\"rejected\":" UINT64_FORMAT
				",\"local_rejected\":" UINT64_FORMAT ",\"order\":\"%s\","
				"\"local_estimated_fraction\":%.6f,"
				"\"transfer_estimated_fraction\":%.6f,"
				"\"cost_source\":\"%s\",\"local_sample_ms\":%.3f,"
				"\"filter_sample_ms\":%.3f,\"local_first_score\":%.3f,"
				"\"filter_first_score\":%.3f}",
				state->prefilter_scans, state->prefilter_rows,
				state->prefilter_rejected, state->local_rejected,
				state->local_first_scans > 0 ? "local-first" : "filter-first",
				state->prefilter_local_fraction,
				state->prefilter_transfer_fraction,
				state->prefilter_costed ? "sample" : "fallback",
				state->prefilter_local_sample_ms,
				state->prefilter_filter_sample_ms,
				state->prefilter_local_first_score,
				state->prefilter_filter_first_score);
			if (state->statistics != NULL)
			{
				ListCell *statistics_cell;
				bool first_key = true;

				appendStringInfo(&profile_relations,
					",\"statistics\":{\"finalize_ms\":%.3f,"
					"\"source\":\"%s\",\"rows\":" UINT64_FORMAT
					",\"rows_examined\":" UINT64_FORMAT
					",\"values_examined\":" UINT64_FORMAT
					",\"average_width\":%d,\"width_source\":\"%s\","
					"\"precomputed_key_sets\":%d,\"scanned_key_sets\":%d,"
					"\"join_keys\":[",
					state->statistics->finalize_ms,
					state->statistics->scanned_key_sets == 0 ?
						"filter" : (state->statistics->vectorized ?
							"vector" : "spill"),
					state->statistics->rows,
					state->statistics->rows_examined,
					state->statistics->values_examined,
					state->statistics->average_width,
					state->statistics->width_estimated ? "store" : "scan",
					state->statistics->precomputed_key_sets,
					state->statistics->scanned_key_sets);
				foreach(statistics_cell, state->statistics->join_keys)
				{
					BloomPGJoinKeyStatistics *keys = lfirst(statistics_cell);
					int key;

					if (!first_key)
						appendStringInfoChar(&profile_relations, ',');
					first_key = false;
					appendStringInfo(&profile_relations,
						"{\"peer_rti\":%u,\"nkeys\":%d,"
						"\"source\":\"%s\","
						"\"nonnull_rows\":" UINT64_FORMAT
						",\"null_rows\":" UINT64_FORMAT
						",\"ndistinct\":%.3f,\"attributes\":[",
						keys->peer_rti, keys->nkeys,
						keys->precomputed ? "filter" : "vector",
						keys->nonnull_rows,
						keys->null_rows, keys->ndistinct);
					for (key = 0; key < keys->nkeys; key++)
					{
						if (key > 0)
							appendStringInfoChar(&profile_relations, ',');
						appendStringInfo(&profile_relations,
							"{\"base_attno\":%d,\"vector_attno\":%d,"
							"\"operator_oid\":%u}",
							keys->base_attributes[key],
							keys->materialized_attributes[key],
							keys->operators[key]);
					}
					appendStringInfoString(&profile_relations, "]}");
				}
				appendStringInfoString(&profile_relations, "]}");
			}
			else
				appendStringInfoString(&profile_relations,
					",\"statistics\":null");
			appendStringInfoChar(&profile_relations, '}');
		}
		if (state->sample_workspace != NULL)
		{
			bloompg_sample_workspace_destroy(state->sample_workspace);
			state->sample_workspace = NULL;
		}
	}
	if (collect_profile)
	{
		StringInfoData profile_json;

		initStringInfo(&profile_json);
		appendStringInfo(&profile_json,
			"{\"summary\":{\"applied\":%s,\"progress_metric\":\"%s\","
			"\"steps\":%d,"
			"\"filters\":%d,\"build_rows\":" UINT64_FORMAT
			",\"equality_domain_classes\":" UINT64_FORMAT
			",\"equality_domain_tracked\":" UINT64_FORMAT
			",\"equality_domain_updates\":" UINT64_FORMAT
			",\"equality_domain_noops\":" UINT64_FORMAT
			",\"equality_domain_suppressed\":" UINT64_FORMAT
			",\"equality_domain_observations\":" UINT64_FORMAT
			",\"equality_domain_uncontained\":" UINT64_FORMAT
			",\"equality_domain_ms\":%.3f"
			",\"statistics_ms\":%.3f,\"statistics_rows\":" UINT64_FORMAT
			",\"statistics_values\":" UINT64_FORMAT
			",\"statistics_key_sets\":%d,\"precomputed_key_sets\":"
			UINT64_FORMAT ",\"scanned_key_sets\":" UINT64_FORMAT
			",\"one_pass_bitmap_builds\":" UINT64_FORMAT
			",\"filter_finalize_rescans\":" UINT64_FORMAT
			",\"filter_finalize_rows\":" UINT64_FORMAT
			",\"index_probe_scans\":" UINT64_FORMAT
			",\"index_probe_batches\":" UINT64_FORMAT
			",\"index_probe_keys\":" UINT64_FORMAT
			",\"index_probe_rows\":" UINT64_FORMAT
			",\"index_probe_ms\":%.3f"
			",\"parallel_scans\":" UINT64_FORMAT
			",\"parallel_workers_requested\":" UINT64_FORMAT
				",\"parallel_workers_launched\":" UINT64_FORMAT
				",\"parallel_rows\":" UINT64_FORMAT
				",\"parallel_ms\":%.3f,\"parallel_waves\":" UINT64_FORMAT
				",\"parallel_append_waves\":" UINT64_FORMAT
				",\"parallel_wave_relations\":" UINT64_FORMAT "},"
			"\"steps\":[%s],\"relations\":[%s]}",
			outcome.applied ? "true" : "false",
			bloompg_transfer_progress_metric ==
				BLOOMPG_TRANSFER_PROGRESS_NDV ? "ndv" : "rows",
			outcome.steps,
			outcome.filters, outcome.build_rows,
			outcome.equality_domain_classes,
			outcome.equality_domain_tracked,
			outcome.equality_domain_updates,
			outcome.equality_domain_noops,
			outcome.equality_domain_suppressed,
			outcome.equality_domain_observations,
			outcome.equality_domain_uncontained,
			outcome.equality_domain_ms, outcome.statistics_ms,
			outcome.statistics_rows, outcome.statistics_values,
			outcome.statistics_key_sets, profile_precomputed_key_sets,
			profile_scanned_key_sets, profile_one_pass_bitmap_builds,
			profile_filter_finalize_rescans,
			profile_filter_finalize_rows, profile_index_probe_scans,
			profile_index_probe_batches, profile_index_probe_keys,
			profile_index_probe_rows,
			profile_index_probe_ms, profile_parallel_scans,
			profile_parallel_workers_requested,
				profile_parallel_workers_launched, profile_parallel_rows,
				profile_parallel_ms, profile_parallel_waves,
				profile_parallel_append_waves,
				profile_parallel_wave_relations, profile_steps.data,
			profile_relations.data);
		outcome.profile_json = profile_json.data;
		pfree(profile_steps.data);
		pfree(profile_relations.data);
	}
	*detail_target = detail;
	return outcome.profile_json;
}
