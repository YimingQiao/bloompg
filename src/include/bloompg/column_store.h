#ifndef BLOOMPG_COLUMN_STORE_H
#define BLOOMPG_COLUMN_STORE_H

#include "postgres.h"

#include "executor/tuptable.h"
#include "utils/resowner.h"

typedef struct BloomPGColumnStore BloomPGColumnStore;

#define BLOOMPG_VECTOR_SIZE 2048
#define BLOOMPG_VALIDITY_BITS 64
#define BLOOMPG_VALIDITY_WORDS \
	((BLOOMPG_VECTOR_SIZE + BLOOMPG_VALIDITY_BITS - 1) / \
	 BLOOMPG_VALIDITY_BITS)

/*
 * A deliberately small subset of DuckDB's physical vector model.  PostgreSQL
 * by-value Datums are stored at their actual width; by-reference Datums are
 * represented by 32-bit offsets into a chunk-local heap.  The latter heap
 * contains PostgreSQL's native datum bytes, so operator functions can consume
 * a pointer into it without conversion.
 */
typedef enum BloomPGVectorPhysicalType
{
	BLOOMPG_VECTOR_BYVAL_1,
	BLOOMPG_VECTOR_BYVAL_2,
	BLOOMPG_VECTOR_BYVAL_4,
	BLOOMPG_VECTOR_BYVAL_8,
	BLOOMPG_VECTOR_BYREF
} BloomPGVectorPhysicalType;

typedef struct BloomPGColumnVector
{
	Oid			type_oid;
	int16		typlen;
	BloomPGVectorPhysicalType physical_type;
	void	   *data;
	uint64	   *validity;
	char	   *heap;
	uint32		heap_used;
	uint32		heap_capacity;
} BloomPGColumnVector;

/* A zero-copy view over one in-memory vector chunk.  Selection entries are
 * physical row indexes into every vector in the chunk.  Views are invalidated
 * when the store is appended to, spilled, filtered, or destroyed. */
typedef struct BloomPGColumnStoreChunkView
{
	uint64		row_base;
	uint32		count;
	uint32		selected_count;
	uint32		vector_count;
	const uint16 *selection;
	const BloomPGColumnVector *vectors;
} BloomPGColumnStoreChunkView;

static inline bool
bloompg_column_vector_is_valid(const BloomPGColumnVector *vector, uint32 row)
{
	Assert(vector != NULL && row < BLOOMPG_VECTOR_SIZE);
	return (vector->validity[row / BLOOMPG_VALIDITY_BITS] &
		(UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS))) != 0;
}

static inline Datum
bloompg_column_vector_get_datum(const BloomPGColumnVector *vector, uint32 row)
{
	Assert(vector != NULL && row < BLOOMPG_VECTOR_SIZE);
	switch (vector->physical_type)
	{
		case BLOOMPG_VECTOR_BYVAL_1:
			return (Datum) ((const uint8 *) vector->data)[row];
		case BLOOMPG_VECTOR_BYVAL_2:
			return (Datum) ((const uint16 *) vector->data)[row];
		case BLOOMPG_VECTOR_BYVAL_4:
			return (Datum) ((const uint32 *) vector->data)[row];
		case BLOOMPG_VECTOR_BYVAL_8:
			return (Datum) ((const uint64 *) vector->data)[row];
		case BLOOMPG_VECTOR_BYREF:
			return PointerGetDatum(vector->heap +
				((const uint32 *) vector->data)[row]);
	}
	pg_unreachable();
}

static inline bool
bloompg_column_chunk_get_datum(const BloomPGColumnStoreChunkView *view,
	AttrNumber attribute, uint32 row, Datum *value)
{
	const BloomPGColumnVector *vector;

	if (!AttributeNumberIsValid(attribute) ||
		(uint32) attribute > view->vector_count || row >= view->count)
		elog(ERROR, "invalid bloompg vector attribute or row");
	vector = &view->vectors[attribute - 1];
	if (!bloompg_column_vector_is_valid(vector, row))
		return false;
	*value = bloompg_column_vector_get_datum(vector, row);
	return true;
}

typedef struct BloomPGColumnStoreScan
{
	int			read_pointer;
	uint64		position;
	TupleTableSlot *spill_slot;
	uint32		memory_chunk;
	bool		memory_chunk_valid;
} BloomPGColumnStoreScan;

extern BloomPGColumnStore *bloompg_column_store_create(MemoryContext parent,
	ResourceOwner runtime_owner, Size memory_limit);
extern void bloompg_column_store_set_descriptor(BloomPGColumnStore *store,
	TupleDesc descriptor);
extern void bloompg_column_store_append_slot(BloomPGColumnStore *store,
	TupleTableSlot *slot);
extern void bloompg_column_store_begin_scan(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan, int eflags);
extern void bloompg_column_store_rescan(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan);
extern bool bloompg_column_store_next(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan, bool forward, TupleTableSlot *slot,
	uint64 *row_index);
extern uint32 bloompg_column_store_chunk_count(
	const BloomPGColumnStore *store);
extern bool bloompg_column_store_chunk_view(const BloomPGColumnStore *store,
	uint32 chunk_index, BloomPGColumnStoreChunkView *view);
extern void bloompg_column_store_set_chunk_selection(
	BloomPGColumnStore *store, uint32 chunk_index, const uint16 *selection,
	uint32 selected_count);
extern void bloompg_column_store_end_scan(BloomPGColumnStoreScan *scan);
extern void bloompg_column_store_deactivate(BloomPGColumnStore *store,
	uint64 row_index);
extern void bloompg_column_store_deactivate_all(BloomPGColumnStore *store);
extern uint64 bloompg_column_store_rows(const BloomPGColumnStore *store);
extern uint64 bloompg_column_store_live_rows(const BloomPGColumnStore *store);
extern bool bloompg_column_store_spilled(const BloomPGColumnStore *store);
extern Size bloompg_column_store_memory_bytes(const BloomPGColumnStore *store);
extern Size bloompg_column_store_peak_memory_bytes(
	const BloomPGColumnStore *store);
extern Size bloompg_column_store_memory_limit(const BloomPGColumnStore *store);
extern uint64 bloompg_column_store_spill_rows(const BloomPGColumnStore *store);
extern uint32 bloompg_column_store_vector_chunks(
	const BloomPGColumnStore *store);
extern uint64 bloompg_column_store_varlen_values(
	const BloomPGColumnStore *store);
extern Size bloompg_column_store_varlen_bytes(
	const BloomPGColumnStore *store);
extern uint64 bloompg_column_store_selection_rewrites(
	const BloomPGColumnStore *store);
extern uint64 bloompg_column_store_selection_rows_examined(
	const BloomPGColumnStore *store);
extern uint64 bloompg_column_store_selection_rows_rejected(
	const BloomPGColumnStore *store);
extern uint64 bloompg_column_store_slot_reconstructions(
	const BloomPGColumnStore *store);
extern void bloompg_column_store_destroy(BloomPGColumnStore *store);

#endif
