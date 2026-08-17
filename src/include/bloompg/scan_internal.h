#ifndef BLOOMPG_SCAN_INTERNAL_H
#define BLOOMPG_SCAN_INTERNAL_H

/* Private executor contracts shared by BloomPG CustomScan implementations. */

#include "postgres.h"

#include "bloompg/scan.h"
#include "bloompg/runtime.h"

#include "access/htup_details.h"
#include "executor/nodeCustom.h"
#include "nodes/extensible.h"
#include "port/atomics.h"
#include "storage/shm_toc.h"

typedef enum BloomPGParallelStoreFormat
{
	BLOOMPG_PARALLEL_STORE_NONE,
	BLOOMPG_PARALLEL_STORE_COLUMNS
} BloomPGParallelStoreFormat;

typedef struct BloomPGParallelColumnVector
{
	Oid			type_oid;
	int16		typlen;
	uint8		physical_type;
	uint32		heap_used;
	Size		data_offset;
	Size		validity_offset;
	Size		heap_offset;
} BloomPGParallelColumnVector;

typedef struct BloomPGParallelColumnChunk
{
	uint32		count;
	uint32		selected_count;
	uint32		vector_count;
	Size		selection_offset;
	BloomPGParallelColumnVector vectors[FLEXIBLE_ARRAY_MEMBER];
} BloomPGParallelColumnChunk;

typedef struct BloomPGParallelStore
{
	pg_atomic_uint64 next_item;
	BloomPGParallelStoreFormat format;
	uint64		rows;
	uint32		chunks;
	uint32		natts;
	Size		total_size;
	Size		data_offset;
	Size		data_bytes;
	bool		available;
	uint64		offsets[FLEXIBLE_ARRAY_MEMBER];
} BloomPGParallelStore;

typedef struct BloomPGParallelCursor
{
	uint64		chunk_index;
	uint32		selected_index;
	bool		chunk_valid;
} BloomPGParallelCursor;

typedef struct BloomPGApplyState
{
	CustomScanState css;
	PlanState  *probe_state;
	PlanState  *build_state;
	BloomPGFilter *filter;
	BloomPGApplySpec spec;
	bool		filter_ready;
} BloomPGApplyState;

typedef struct BloomPGMemoryState
{
	CustomScanState css;
	PlanState  *fallback_state;
	BloomPGRuntimeMaterializationEntry *entry;
	uint64		token;
	uint64		planned_rows;
	BloomPGColumnStoreScan store_scan;
	BloomPGParallelStore *parallel_store;
	BloomPGParallelCursor parallel_cursor;
	Size		parallel_size;
	bool		parallel_active;
	bool		snapshot_reused;
} BloomPGMemoryState;

typedef struct BloomPGTidState
{
	CustomScanState css;
	PlanState  *fallback_state;
	BloomPGRuntimeMaterializationEntry *entry;
	TupleTableSlot *tid_slot;
	TupleTableSlot *heap_slot;
	uint64		token;
	uint64		planned_rows;
	BloomPGColumnStoreScan store_scan;
	BloomPGParallelStore *parallel_store;
	BloomPGParallelCursor parallel_cursor;
	Size		parallel_size;
	bool		parallel_active;
} BloomPGTidState;

typedef struct BloomPGExecFilter
{
	uint64		token;
	int			nkeys;
	AttrNumber *attributes;
	BloomPGFilter *filter;
} BloomPGExecFilter;

typedef struct BloomPGParallelFilterKey
{
	Oid			probe_type;
	Oid			collation;
	Oid			probe_hash_proc;
} BloomPGParallelFilterKey;

typedef struct BloomPGParallelFilterEntry
{
	bool		active;
	int			kind;
	int			nkeys;
	int			hash_functions;
	uint64		nbits;
	uint64		bit_mask;
	uint64		inserted;
	int64		bitmap_lower;
	int64		bitmap_upper;
	Size		keys_offset;
	Size		words_offset;
} BloomPGParallelFilterEntry;

typedef struct BloomPGParallelFilterStore
{
	bool		available;
	int			nfilters;
	Size		total_size;
	BloomPGParallelFilterEntry entries[FLEXIBLE_ARRAY_MEMBER];
} BloomPGParallelFilterStore;

typedef struct BloomPGFilterState
{
	CustomScanState css;
	PlanState  *fallback_state;
	List	   *filters;
	uint64		rejected;
	uint64		rescans;
	uint64		parameter_rescans;
	BloomPGParallelFilterStore *parallel_store;
	Size		parallel_size;
	bool		parallel_active;
	bool		prefilter;
} BloomPGFilterState;

extern Size bloompg_parallel_filter_header_size(int nfilters);
extern Size bloompg_parallel_filter_add_size(Size size,
	const BloomPGFilter *filter);

extern const CustomScanMethods bloompg_apply_scan_methods;
extern const CustomScanMethods bloompg_memory_scan_methods;
extern const CustomScanMethods bloompg_empty_scan_methods;
extern const CustomScanMethods bloompg_tid_scan_methods;
extern const CustomScanMethods bloompg_filter_scan_methods;
extern const CustomScanMethods bloompg_prefilter_scan_methods;
extern const CustomScanMethods bloompg_pack_scan_methods;

extern Oid *bloompg_list_to_oid_array(List *values, int expected);
extern AttrNumber *bloompg_list_to_attr_array(List *values, int expected);

extern Size bloompg_estimate_parallel_store(
	BloomPGRuntimeMaterializationEntry *entry);
extern void bloompg_fill_parallel_store(
	BloomPGRuntimeMaterializationEntry *entry,
	BloomPGParallelStore *parallel_store, Size allocated);
extern bool bloompg_parallel_next_column(BloomPGParallelStore *store,
	BloomPGParallelCursor *cursor, TupleTableSlot *slot);

#endif
