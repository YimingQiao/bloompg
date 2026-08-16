#ifndef BLOOMPG_RUNTIME_H
#define BLOOMPG_RUNTIME_H

#include "postgres.h"

#include "bloompg/column_store.h"
#include "bloompg/filter.h"
#include "utils/snapshot.h"

typedef struct BloomPGRuntimeFilterEntry
{
	uint64		token;
	uint64		snapshot_signature;
	BloomPGFilter *filter;
} BloomPGRuntimeFilterEntry;

typedef struct BloomPGRuntimeMaterializationEntry
{
	uint64		token;
	uint64		snapshot_signature;
	BloomPGColumnStore *store;
	TupleDesc	descriptor;
	uint64		rows;
} BloomPGRuntimeMaterializationEntry;

extern uint64 bloompg_runtime_next_token(void);
extern uint64 bloompg_snapshot_signature(Snapshot snapshot);
extern BloomPGRuntimeFilterEntry *bloompg_runtime_find_filter(uint64 token,
														 Snapshot snapshot);
extern BloomPGRuntimeFilterEntry *bloompg_runtime_register_filter(uint64 token,
											Snapshot snapshot,
											BloomPGFilter *filter);
extern void bloompg_runtime_release_filter(uint64 token);
extern BloomPGRuntimeMaterializationEntry *
bloompg_runtime_find_materialization(uint64 token, Snapshot snapshot);
extern BloomPGRuntimeMaterializationEntry *
bloompg_runtime_register_materialization(uint64 token,
										 Snapshot snapshot,
										 BloomPGColumnStore *store,
										 TupleDesc descriptor,
										 uint64 rows);
extern void bloompg_runtime_release_materialization(uint64 token);
extern void bloompg_runtime_cleanup_since(uint64 first_token);

#endif
