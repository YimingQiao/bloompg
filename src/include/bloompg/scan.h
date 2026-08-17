#ifndef BLOOMPG_SCAN_H
#define BLOOMPG_SCAN_H

#include "postgres.h"

#include "bloompg/column_store.h"

#include "access/tupdesc.h"
#include "nodes/plannodes.h"

typedef void (*BloomPGPackedChunkVisitor)(
	const BloomPGColumnVector *vectors,
	uint32 vector_count,
	uint32 count,
	void *context);

typedef struct BloomPGApplySpec
{
	uint64		token;
	int64		estimated_rows;
	int			nkeys;
	Oid		   *operators;
	Oid		   *collations;
	AttrNumber *build_attributes;
	AttrNumber *probe_attributes;
	AttrNumber *probe_base_attributes;
	bool		build_is_left;
} BloomPGApplySpec;

extern Plan *bloompg_make_apply_scan(Plan *probe_plan,
									 Plan *build_plan,
									 const BloomPGApplySpec *spec,
									 int plan_node_id);
extern Plan *bloompg_make_memory_scan(Plan *fallback_plan,
									  uint64 token,
									  uint64 rows,
									  int plan_node_id);
extern Plan *bloompg_make_memory_scan_path(List *targetlist,
									   List *scan_targetlist,
									   Plan *fallback_plan,
									   uint64 token,
									   uint64 rows);
extern bool bloompg_memory_scan_info(Plan *plan, uint64 *token, uint64 *rows);
extern Plan *bloompg_make_tid_scan_path(List *targetlist,
									 Plan *fallback_plan,
									 Index rti,
									 uint64 token,
									 uint64 rows);
extern bool bloompg_tid_scan_info(Plan *plan, uint64 *token, uint64 *rows);
extern Plan *bloompg_make_filter_scan_path(List *targetlist,
									List *scan_targetlist,
									Plan *fallback_plan,
									List *filter_metadata);
extern Plan *bloompg_make_filter_scan(Plan *fallback_plan,
								  List *filter_metadata,
								  int plan_node_id,
								  bool parallel_aware,
								  bool prefilter);
extern Plan *bloompg_make_pack_scan(Plan *fallback_plan,
	int plan_node_id, int parallel_workers);
extern Size bloompg_parallel_pack_memory(int parallel_workers);
extern uint32 bloompg_receive_packed_chunk(Datum datum,
	BloomPGColumnStore *store, TupleDesc descriptor,
	BloomPGPackedChunkVisitor visitor, void *visitor_context);
extern bool bloompg_filter_scan_uses_token(Plan *plan, uint64 token);
extern Plan *bloompg_make_empty_scan_path(List *targetlist,
								   List *scan_targetlist,
								   Plan *fallback_plan,
								   uint64 token);
extern bool bloompg_empty_scan_info(Plan *plan, uint64 *token);

#endif
