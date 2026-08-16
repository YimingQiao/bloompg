#ifndef BLOOMPG_FILTER_H
#define BLOOMPG_FILTER_H

#include "postgres.h"

#include "bloompg/column_store.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "fmgr.h"

typedef enum BloomPGFilterKind
{
	BLOOMPG_FILTER_BLOOM,
	BLOOMPG_FILTER_BITMAP
} BloomPGFilterKind;

typedef struct BloomPGFilterKey
{
	Oid			build_type;
	Oid			probe_type;
	Oid			collation;
	Oid			equality_operator;
	Oid			build_hash_proc;
	Oid			probe_hash_proc;
	FmgrInfo	build_hash;
	FmgrInfo	probe_hash;
	bool		strict;
} BloomPGFilterKey;

typedef struct BloomPGFilter
{
	BloomPGFilterKind kind;
	MemoryContext memory_context;
	uint32		refcount;
	int			nkeys;
	BloomPGFilterKey *keys;
	bool		valid;

	uint64		nbits;
	uint64		bit_mask;
	uint64	   *words;
	int			hash_functions;

	int64		bitmap_lower;
	int64		bitmap_upper;

	uint64		inserted;
	uint64		lookups;
	uint64		rejected;
} BloomPGFilter;

extern BloomPGFilter *bloompg_filter_create_bloom(MemoryContext context,
												   int64 estimated_rows,
												   int nkeys,
												   const Oid *operators,
												   const Oid *collations,
												   bool build_is_left);
extern BloomPGFilter *bloompg_filter_create_bitmap(MemoryContext context,
													int64 lower,
													int64 upper,
													Oid operator_oid,
													Oid collation,
													bool build_is_left);
extern void bloompg_filter_insert_slot(BloomPGFilter *filter,
										 TupleTableSlot *slot,
										 const AttrNumber *attributes);
extern bool bloompg_filter_contains_slot(BloomPGFilter *filter,
										  TupleTableSlot *slot,
										  const AttrNumber *attributes);
extern void bloompg_filter_insert_vector(BloomPGFilter *filter,
	const BloomPGColumnStoreChunkView *view, uint32 row,
	const AttrNumber *attributes);
extern bool bloompg_filter_contains_vector(BloomPGFilter *filter,
	const BloomPGColumnStoreChunkView *view, uint32 row,
	const AttrNumber *attributes);

static inline void
bloompg_filter_insert_bitmap_value(BloomPGFilter *filter, Datum value,
	bool is_null)
{
	int64 integral;
	uint64 offset;

	Assert(filter != NULL && filter->kind == BLOOMPG_FILTER_BITMAP &&
		filter->nkeys == 1);
	if (is_null)
		return;
	switch (filter->keys[0].build_type)
	{
		case INT2OID:
			integral = DatumGetInt16(value);
			break;
		case INT4OID:
		case DATEOID:
			integral = DatumGetInt32(value);
			break;
		case INT8OID:
			integral = DatumGetInt64(value);
			break;
		case OIDOID:
			integral = (int64) DatumGetObjectId(value);
			break;
		default:
			return;
	}
	if (integral < filter->bitmap_lower || integral > filter->bitmap_upper)
		return;
	offset = (uint64) integral - (uint64) filter->bitmap_lower;
	filter->words[offset >> 6] |= UINT64CONST(1) << (offset & 63);
	filter->inserted++;
}

/* Hot path for the overwhelmingly common Bloom case: a canonical integral
 * equality has already been converted to a one-key exact bitmap. */
static inline bool
bloompg_filter_contains_bitmap_value(BloomPGFilter *filter, Datum value,
	bool is_null)
{
	int64 integral;
	uint64 offset;
	bool result;

	Assert(filter != NULL && filter->kind == BLOOMPG_FILTER_BITMAP &&
		filter->nkeys == 1);
	filter->lookups++;
	if (!filter->valid)
		return true;
	if (is_null)
		result = false;
	else
	{
		switch (filter->keys[0].probe_type)
		{
			case INT2OID:
				integral = DatumGetInt16(value);
				break;
			case INT4OID:
			case DATEOID:
				integral = DatumGetInt32(value);
				break;
			case INT8OID:
				integral = DatumGetInt64(value);
				break;
			case OIDOID:
				integral = (int64) DatumGetObjectId(value);
				break;
			default:
				integral = 0;
				is_null = true;
				break;
		}
		result = !is_null && integral >= filter->bitmap_lower &&
			integral <= filter->bitmap_upper;
		if (result)
		{
			offset = (uint64) integral - (uint64) filter->bitmap_lower;
			result = (filter->words[offset >> 6] &
				(UINT64CONST(1) << (offset & 63))) != 0;
		}
	}
	if (!result)
		filter->rejected++;
	return result;
}
extern void bloompg_filter_set_valid(BloomPGFilter *filter);
extern BloomPGFilter *bloompg_filter_retain(BloomPGFilter *filter);
extern void bloompg_filter_free(BloomPGFilter *filter);
extern bool bloompg_filter_exact_statistics(const BloomPGFilter *filter,
	uint64 rows, uint64 *nonnull_rows, uint64 *null_rows,
	double *ndistinct);

#endif
