#ifndef BLOOMPG_TRANSFER_INTERNAL_H
#define BLOOMPG_TRANSFER_INTERNAL_H

/* Private contracts shared by transfer implementation modules. */

#include "postgres.h"

#include "nodes/params.h"
#include "bloompg/scan.h"
#include "bloompg/graph.h"
#include "bloompg/runtime.h"
#include "bloompg/sampling.h"
#include "bloompg/statistics.h"
#include "bloompg/transfer.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/plannodes.h"
#include "portability/instr_time.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/snapshot.h"

#define BLOOMPG_MAX_INDEX_BITMAP_SOURCES 3

typedef struct BloomPGEqualityDomain BloomPGEqualityDomain;
typedef struct BloomPGEqualityDomains BloomPGEqualityDomains;

typedef struct BloomPGIndexBitmapInputProfile
{
	Oid			index_oid;
	AttrNumber	attribute;
	uint64		filter_token;
	uint64		keys;
	double		estimated_fraction;
	uint64		index_tuples;
} BloomPGIndexBitmapInputProfile;

typedef struct BloomPGRelationState
{
	Index		rti;
	Index		replacement_rti;
	BloomPGTable *table;
	Query	   *analysis_query;
	int			cursor_options;
	Plan	   *plan;
	Plan	   *fallback_plan;
	Index	   *local_to_global;
	int			local_rtable_count;
	Bitmapset  *scope_relids;
	bool		simple_subquery;
	AttrNumber	ctid_attribute;
	bool		tid_eligible;
	double		base_rows;
	double		current_rows;
	double		local_rows;
	double		filter_sample_fraction;
	bool		local_sampled;
	double		excitation_rows;
	Bitmapset  *lineage;
	bool		active;
	bool		native_parameterized_index;
	bool		native_index_guarded;
	bool		materialized;
	bool		prefetched;
	bool		changed;
	bool		materialization_used_filters;
	uint64		materialization_token;
	uint64		prefetch_token;
	uint64		prefetch_rows;
	List	   *filters;
	BloomPGSampleWorkspace *sample_workspace;
	BloomPGSamplingProfile sampling_profile;
	BloomPGIntermediateSamplingProfile intermediate_sampling_profile;
	uint64		exact_scans;
	double		exact_scan_ms;
	uint64		base_materializations;
	uint64		compactions;
	double		base_materialize_ms;
	double		compact_ms;
	uint64		parallel_scans;
	uint64		parallel_workers_requested;
	uint64		parallel_workers_launched;
	uint64		parallel_rows;
	double		parallel_scan_ms;
	/* A speculative pair can share one physical PostgreSQL worker wave. */
	uint64		parallel_wave_id;
	const char *parallel_wave_kind;
	const char *parallel_wave_layout;
	bool		parallel_wave_coordinator;
	Index		parallel_wave_peer_rti;
	uint64		parallel_wave_workers_requested;
	uint64		parallel_wave_workers_launched;
	double		parallel_wave_ms;
	uint64		prefilter_scans;
	uint64		prefilter_rows;
	uint64		prefilter_rejected;
	uint64		local_rejected;
	uint64		local_first_scans;
	double		prefilter_local_fraction;
	double		prefilter_transfer_fraction;
	double		prefilter_local_sample_ms;
	double		prefilter_filter_sample_ms;
	double		prefilter_local_first_score;
	double		prefilter_filter_first_score;
	bool		prefilter_costed;
	uint64		index_probe_scans;
	uint64		index_probe_batches;
	uint64		index_probe_keys;
	uint64		index_probe_rows;
	double		index_probe_ms;
	double		index_probe_fraction;
	Oid			index_probe_oid;
	AttrNumber	index_probe_attribute;
	uint64		index_probe_filter_token;
	uint64		index_probe_candidates;
	uint64		index_probe_sample_rows;
	uint64		index_probe_sample_matches;
	double		index_probe_estimate_ms;
	const char *index_probe_estimate_source;
	uint64		index_bitmap_scans;
	uint64		index_bitmap_sources;
	uint64		index_bitmap_index_tuples;
	uint64		index_bitmap_exact_pages;
	uint64		index_bitmap_lossy_pages;
	uint64		index_probe_dominated_candidates;
	int			index_bitmap_input_count;
	BloomPGIndexBitmapInputProfile
				index_bitmap_inputs[BLOOMPG_MAX_INDEX_BITMAP_SOURCES];
	const char *materialization_access_path;
	uint64		access_plan_attempts;
	double		access_plan_ms;
	uint64		access_plan_exact_filters;
	uint64		access_plan_exact_keys;
	double		access_plan_startup_cost;
	double		access_plan_total_cost;
	double		access_plan_rows;
	uint64		access_plan_seq_scans;
	uint64		access_plan_index_scans;
	uint64		access_plan_index_only_scans;
	uint64		access_plan_bitmap_heap_scans;
	uint64		access_plan_bitmap_index_scans;
	uint64		access_plan_gathers;
	const char *access_plan_kind;
	const char *access_plan_rejection;
	/* Static join-key index opportunities and the last guard decision. */
	Bitmapset  *join_index_sources;
	bool		selection_index_ready;
	bool		selection_guarded;
	bool		selection_guard_fallback;
	bool		selection_native_fallback;
	uint64		column_compactions;
	uint64		slot_compactions;
	uint64		vector_filter_builds;
	uint64		slot_filter_builds;
	double		filter_prepare_ms;
	double		filter_finalize_ms;
	double		filter_build_ms;
	uint64		one_pass_bitmap_builds;
	uint64		filter_finalize_rescans;
	uint64		filter_finalize_rows;
	int			original_output_columns;
	int			materialized_output_columns;
	const char *replacement_kind;
	BloomPGMaterializedStatistics *statistics;
	List	   *built_key_statistics;
} BloomPGRelationState;

typedef struct BloomPGPendingTransfer
{
	BloomPGRelationState *destination;
	BloomPGApplySpec spec;
	BloomPGFilter *filter;
	Bitmapset **sent_lineage;
	bool		integral_key;
	bool		has_range;
	int64		minimum;
	int64		maximum;
	bool		build_filter;
	bool		shared_filter;
	uint64	   *bitmap_candidate_words;
	uint64		bitmap_candidate_lower_word;
	uint64		bitmap_candidate_upper_word;
	uint64		bitmap_candidate_inserted;
	bool		bitmap_candidate_abandoned;
	bool		bitmap_one_pass;
	/* Exact, same-type integral equality-class propagation state. */
	BloomPGEqualityDomain *equality_domain;
	bool		exact_ndistinct_valid;
	uint64		exact_ndistinct;
	uint64		equality_domain_version;
} BloomPGPendingTransfer;

static inline double
bloompg_transfer_elapsed_ms(instr_time started)
{
	instr_time elapsed;

	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	return INSTR_TIME_GET_MILLISEC(elapsed);
}

/* Physical scan/materialization helpers (transfer_materialize.c). */
extern BloomPGColumnStore *bloompg_begin_runtime_store(
	ResourceOwner runtime_owner);
extern bool bloompg_integral_value(Datum datum, Oid type_oid, int64 *result);
extern bool bloompg_process_pending_slot(List *pending_transfers,
	TupleTableSlot *slot);
extern void bloompg_process_pending_columns(List *pending_transfers,
	const BloomPGColumnStoreChunkView *view, uint32 row);
extern bool bloompg_contains_materialized_filter(BloomPGFilter *filter,
	const BloomPGColumnStoreChunkView *view, uint32 row,
	const AttrNumber *attributes);
extern void bloompg_insert_materialized_filter(BloomPGFilter *filter,
	const BloomPGColumnStoreChunkView *view, uint32 row,
	const AttrNumber *attributes);
extern bool bloompg_build_pending_from_materialization(List *pending,
	BloomPGColumnStore *store, TupleDesc descriptor);
extern void bloompg_discard_pending_filters(List *pending_transfers);
extern uint64 bloompg_compact_materialization(BloomPGRelationState *state,
	List *pending_transfers, TupleDesc *descriptor);
extern uint64 bloompg_finalize_pending_filters(List *pending_transfers,
	BloomPGColumnStore *store, TupleDesc descriptor, uint64 rows,
	uint64 *one_pass_bitmap_builds);
extern uint64 bloompg_create_tid_materialization(
	BloomPGRuntimeMaterializationEntry *source, AttrNumber ctid_attribute,
	uint64 expected_rows, ResourceOwner runtime_owner);
extern uint64 bloompg_execute_state_source(BloomPGRelationState *state,
	PlannedStmt *candidate, Plan *source_plan, List *pending,
	BloomPGColumnStore *store, TupleDesc *descriptor,
	const char *query_string, ParamListInfo bound_params,
	bool query_parallel_safe, int *next_plan_node_id);
extern bool bloompg_plan_transfer_access(BloomPGRelationState *state,
	const char *query_string, ParamListInfo bound_params,
	PlannedStmt **result);
extern int bloompg_transfer_worker_count(BloomPGRelationState *state,
	bool query_parallel_safe);
extern Plan *bloompg_make_parallel_source(BloomPGRelationState *state,
	int workers, int *next_plan_node_id);

/* Exact equality-domain propagation deduplication (transfer_domain.c). */
extern BloomPGEqualityDomains *bloompg_equality_domains_create(
	const BloomPGJoinGraph *graph, int relation_count);
extern BloomPGEqualityDomain *bloompg_equality_domain_for_transfer(
	BloomPGEqualityDomains *domains, Index source_rti, Index destination_rti,
	int nkeys);
extern void bloompg_equality_domains_prepare_source(
	BloomPGEqualityDomains *domains, BloomPGRelationState *source,
	List *pending_transfers);
extern bool bloompg_equality_domain_suppresses(
	BloomPGEqualityDomains *domains, BloomPGRelationState *source,
	BloomPGPendingTransfer *pending);
extern void bloompg_equality_domain_mark_applied(
	BloomPGEqualityDomains *domains, BloomPGPendingTransfer *pending,
	BloomPGRelationState *destination);
extern void bloompg_equality_domains_finish(BloomPGEqualityDomains *domains,
	BloomPGTransferOutcome *outcome, StringInfo detail);
extern bool bloompg_batch_targetlists_permutable(Plan *source, Plan *peer,
	int **common_to_peer, int *columns);
extern bool bloompg_execute_parallel_batch(BloomPGRelationState *source,
	BloomPGRelationState *peer, PlannedStmt *candidate,
	BloomPGColumnStore *source_store, BloomPGColumnStore *peer_store,
	const char *query_string, ParamListInfo bound_params, int workers,
	int *next_plan_node_id, uint64 *source_rows, uint64 *peer_rows,
	TupleDesc *source_descriptor, TupleDesc *peer_descriptor,
	int *workers_launched, double *scan_ms, const char **layout);
extern bool bloompg_prefetch_active_pair(BloomPGRelationState *source,
	BloomPGRelationState **states, int relation_count,
	const BloomPGJoinGraph *graph, PlannedStmt *candidate,
	const char *query_string,
	ParamListInfo bound_params, ResourceOwner runtime_owner,
	bool query_parallel_safe, int *next_plan_node_id,
	uint64 *next_parallel_wave_id);

/* Fixed-point completion, post-transfer statistics, and diagnostics. */
extern void bloompg_remember_built_key_statistics(
	BloomPGRelationState *state, BloomPGPendingTransfer *pending, uint64 rows);
extern BloomPGMaterializedStatistics *bloompg_finalize_state_statistics(
	BloomPGRelationState *state, BloomPGRelationState **states,
	int relation_count, const BloomPGJoinGraph *graph,
	BloomPGRuntimeMaterializationEntry *entry);
extern void bloompg_finalize_replacements(PlannedStmt *candidate,
	BloomPGJoinGraph *graph, BloomPGRelationState **states,
	int relation_count, const char *query_string,
	ParamListInfo bound_params, ResourceOwner runtime_owner,
	bool query_parallel_safe, int *next_plan_node_id,
	BloomPGTransferOutcome *outcome, StringInfoData *detail);
extern void bloompg_transfer_profile_append_step(StringInfo json, int step,
	BloomPGRelationState *source, BloomPGPendingTransfer *pending,
	uint64 rows_seen, uint64 materialization_token);
extern char *bloompg_transfer_finish_profile(
	BloomPGRelationState **states, int relation_count,
	BloomPGTransferOutcome outcome, StringInfoData *detail,
	StringInfoData *profile_steps);

/* Exact-bitmap-driven B-tree access (transfer_index.c). */
typedef bool (*BloomPGIndexTupleCallback)(TupleTableSlot *slot, void *context);
extern bool bloompg_index_prefilter_available(BloomPGRelationState *state);
extern bool bloompg_index_prefilter_estimate(BloomPGRelationState *state,
	double *fraction, uint64 *keys, Oid *index_oid);
extern bool bloompg_relation_has_transfer_index(BloomPGRelationState *state,
	AttrNumber attribute, Oid equality_operator, Oid collation);
extern bool bloompg_index_prefilter_scan(BloomPGRelationState *state,
	Relation heap_relation, Snapshot snapshot,
	BloomPGIndexTupleCallback callback, void *callback_context);

/* Plan-tree discovery and rewrite helpers (transfer_plan.c). */
extern Plan *bloompg_find_scan(Plan *plan, Index wanted_rti);
extern Bitmapset *bloompg_plan_relids(Plan *plan);
extern Plan *bloompg_find_scope_root(Plan *plan,
	const Bitmapset *scope_relids);
extern bool bloompg_map_subquery(PlannedStmt *candidate,
	BloomPGJoinGraph *graph, BloomPGRelationState *state,
	Bitmapset *main_relids, Bitmapset **used_relids);
extern bool bloompg_simple_subquery(Query *query);
extern bool bloompg_plan_has_parallel_node(Plan *plan);
extern int bloompg_max_plan_node_id(Plan *plan);
extern Plan *bloompg_replace_plan_pointer(Plan *plan, Plan *wanted,
	Plan *replacement, bool *done);
extern List *bloompg_semantic_targetlist(Plan *plan);
extern AttrNumber bloompg_prepare_tid_output(Plan *plan, Index rti,
	const BloomPGJoinGraph *graph);
extern bool bloompg_prepare_projected_output(Plan *plan, BloomPGTable *table,
	const BloomPGJoinGraph *graph, int *original_columns,
	int *projected_columns);
extern void bloompg_profile_append_projection_attributes(StringInfo json,
	BloomPGRelationState *state);
extern Node *bloompg_normalize_syntax_vars(Node *node, void *context);
extern AttrNumber bloompg_find_state_output_attribute(
	BloomPGRelationState *state, BloomPGColumnRef column);
extern List *bloompg_subquery_replacement_targetlist(
	BloomPGRelationState *state);
extern double bloompg_relation_rows(PlannedStmt *candidate, Index rti,
	Plan *scan);
extern void bloompg_disable_parallel_flags(Plan *plan);

#endif
