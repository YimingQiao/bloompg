#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/filter.h"

#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include <math.h>

#define BLOOMPG_MIN_BLOOM_BITS UINT64CONST(512)

static uint64
bloompg_ceil_power_of_two(uint64 value)
{
	uint64 result = 1;

	while (result < value && result < (UINT64CONST(1) << 62))
		result <<= 1;
	return result;
}

static bool
bloompg_integral_datum(Datum datum, Oid type_oid, int64 *result)
{
	switch (type_oid)
	{
		case INT2OID:
			*result = DatumGetInt16(datum);
			return true;
		case INT4OID:
			*result = DatumGetInt32(datum);
			return true;
		case INT8OID:
			*result = DatumGetInt64(datum);
			return true;
		case OIDOID:
			*result = (int64) DatumGetObjectId(datum);
			return true;
		case DATEOID:
			*result = (int64) DatumGetDateADT(datum);
			return true;
		default:
			return false;
	}
}

static void
bloompg_initialize_keys(BloomPGFilter *filter,
						int nkeys,
						const Oid *operators,
						const Oid *collations,
						bool build_is_left)
{
	int i;

	filter->nkeys = nkeys;
	filter->keys = palloc0_array(BloomPGFilterKey, nkeys);
	for (i = 0; i < nkeys; i++)
	{
		BloomPGFilterKey *key = &filter->keys[i];
		RegProcedure left_hash;
		RegProcedure right_hash;
		Oid left_type;
		Oid right_type;

		op_input_types(operators[i], &left_type, &right_type);
		if (!get_op_hash_functions(operators[i], &left_hash, &right_hash))
			elog(ERROR, "bloompg could not resolve hash support for operator %u",
				 operators[i]);

		key->build_type = build_is_left ? left_type : right_type;
		key->probe_type = build_is_left ? right_type : left_type;
		key->build_hash_proc = build_is_left ? left_hash : right_hash;
		key->probe_hash_proc = build_is_left ? right_hash : left_hash;
		key->collation = collations != NULL ? collations[i] : InvalidOid;
		key->equality_operator = operators[i];
		key->strict = op_strict(operators[i]);
		fmgr_info(key->build_hash_proc, &key->build_hash);
		fmgr_info(key->probe_hash_proc, &key->probe_hash);
	}
}

BloomPGFilter *
bloompg_filter_create_bloom(MemoryContext context,
							int64 estimated_rows,
							int nkeys,
							const Oid *operators,
							const Oid *collations,
							bool build_is_left)
{
	BloomPGFilter *filter;
	MemoryContext filter_context;
	MemoryContext old_context;
	uint64 target_bits;
	uint64 max_bits;
	double ideal_hashes;

	Assert(context != NULL);
	Assert(estimated_rows >= 0);
	Assert(nkeys > 0);

	filter_context = AllocSetContextCreate(context, "BloomPG filter",
		ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(filter_context);
	filter = palloc0(sizeof(BloomPGFilter));
	filter->kind = BLOOMPG_FILTER_BLOOM;
	filter->memory_context = filter_context;
	filter->refcount = 1;
	bloompg_initialize_keys(filter, nkeys, operators, collations, build_is_left);

	max_bits = (uint64) bloompg_max_filter_bytes * BITS_PER_BYTE;
	if ((uint64) Max(estimated_rows, INT64CONST(1)) >
		max_bits / (uint64) bloompg_bits_per_key)
		target_bits = max_bits;
	else
		target_bits = (uint64) Max(estimated_rows, INT64CONST(1)) *
			(uint64) bloompg_bits_per_key;
	target_bits = Max(target_bits, BLOOMPG_MIN_BLOOM_BITS);
	target_bits = Min(target_bits, max_bits);
	filter->nbits = bloompg_ceil_power_of_two(target_bits);
	if (filter->nbits > max_bits)
		filter->nbits >>= 1;
	filter->nbits = Max(filter->nbits, BLOOMPG_MIN_BLOOM_BITS);
	filter->bit_mask = filter->nbits - 1;
	filter->words = palloc0_array(uint64, filter->nbits / 64);

	ideal_hashes = log(2.0) * (double) filter->nbits /
		(double) Max(estimated_rows, INT64CONST(1));
	filter->hash_functions = (int) rint(ideal_hashes);
	filter->hash_functions = Max(1, Min(filter->hash_functions,
									  BLOOMPG_MAX_HASH_FUNCTIONS));
	MemoryContextSwitchTo(old_context);
	return filter;
}

BloomPGFilter *
bloompg_filter_create_bitmap(MemoryContext context,
							 int64 lower,
							 int64 upper,
							 Oid operator_oid,
							 Oid collation,
							 bool build_is_left)
{
	BloomPGFilter *filter;
	MemoryContext filter_context;
	MemoryContext old_context;
	uint64 width;

	if (upper < lower)
		elog(ERROR, "bloompg bitmap upper bound is below lower bound");
	width = (uint64) upper - (uint64) lower + 1;
	if (width == 0)
		elog(ERROR, "bloompg bitmap range spans the full 64-bit domain");
	if (width > (uint64) bloompg_max_filter_bytes * BITS_PER_BYTE)
		elog(ERROR, "bloompg bitmap exceeds bloompg.max_filter_bytes");

	filter_context = AllocSetContextCreate(context, "BloomPG bitmap filter",
		ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(filter_context);
	filter = palloc0(sizeof(BloomPGFilter));
	filter->kind = BLOOMPG_FILTER_BITMAP;
	filter->memory_context = filter_context;
	filter->refcount = 1;
	filter->bitmap_lower = lower;
	filter->bitmap_upper = upper;
	filter->nbits = ((width + 63) / 64) * 64;
	filter->words = palloc0_array(uint64, filter->nbits / 64);
	bloompg_initialize_keys(filter, 1, &operator_oid, &collation, build_is_left);
	MemoryContextSwitchTo(old_context);
	return filter;
}

static bool
bloompg_hash_slot(BloomPGFilter *filter,
				  TupleTableSlot *slot,
				  const AttrNumber *attributes,
				  bool build_side,
				  uint64 *result)
{
	uint64 combined = 0;
	int i;

	for (i = 0; i < filter->nkeys; i++)
	{
		BloomPGFilterKey *key = &filter->keys[i];
		bool is_null;
		Datum value = slot_getattr(slot, attributes[i], &is_null);
		FmgrInfo *hash_info = build_side ? &key->build_hash : &key->probe_hash;
		uint32 hash;

		/* Hash joins using strict equality cannot match a NULL join key. */
		if (is_null)
			return false;
		hash = DatumGetUInt32(FunctionCall1Coll(hash_info, key->collation, value));
		if (i == 0)
			combined = murmurhash64((uint64) hash);
		else
			combined = hash_combine64(combined, (uint64) hash);
	}
	*result = combined;
	return true;
}

static bool
bloompg_hash_vector(BloomPGFilter *filter,
					const BloomPGColumnStoreChunkView *view,
					uint32 row,
					const AttrNumber *attributes,
					bool build_side,
					uint64 *result)
{
	uint64 combined = 0;
	int i;

	for (i = 0; i < filter->nkeys; i++)
	{
		BloomPGFilterKey *key = &filter->keys[i];
		Datum value;
		FmgrInfo *hash_info;
		uint32 hash;

		if (!AttributeNumberIsValid(attributes[i]))
			elog(ERROR, "invalid bloompg column-store filter attribute");
		if (!bloompg_column_chunk_get_datum(view, attributes[i], row, &value))
			return false;
		hash_info = build_side ? &key->build_hash : &key->probe_hash;
		hash = DatumGetUInt32(FunctionCall1Coll(hash_info, key->collation, value));
		if (i == 0)
			combined = murmurhash64((uint64) hash);
		else
			combined = hash_combine64(combined, (uint64) hash);
	}
	*result = combined;
	return true;
}

static inline void
bloompg_bloom_insert_hash(BloomPGFilter *filter, uint64 hash)
{
	uint64 first = murmurhash64(hash);
	uint64 second = murmurhash64(hash ^ UINT64CONST(0x9e3779b97f4a7c15)) | 1;
	int i;

	for (i = 0; i < filter->hash_functions; i++)
	{
		uint64 bit = (first + (uint64) i * second + (uint64) i * (uint64) i) &
			filter->bit_mask;

		filter->words[bit >> 6] |= UINT64CONST(1) << (bit & 63);
	}
}

static inline bool
bloompg_bloom_contains_hash(BloomPGFilter *filter, uint64 hash)
{
	uint64 first = murmurhash64(hash);
	uint64 second = murmurhash64(hash ^ UINT64CONST(0x9e3779b97f4a7c15)) | 1;
	int i;

	for (i = 0; i < filter->hash_functions; i++)
	{
		uint64 bit = (first + (uint64) i * second + (uint64) i * (uint64) i) &
			filter->bit_mask;

		if ((filter->words[bit >> 6] & (UINT64CONST(1) << (bit & 63))) == 0)
			return false;
	}
	return true;
}

void
bloompg_filter_insert_slot(BloomPGFilter *filter,
						   TupleTableSlot *slot,
						   const AttrNumber *attributes)
{
	if (filter->kind == BLOOMPG_FILTER_BLOOM)
	{
		uint64 hash;

		if (!bloompg_hash_slot(filter, slot, attributes, true, &hash))
			return;
		bloompg_bloom_insert_hash(filter, hash);
	}
	else
	{
		bool is_null;
		Datum value = slot_getattr(slot, attributes[0], &is_null);
		int64 integral;
		uint64 offset;

		if (is_null || !bloompg_integral_datum(value,
											 filter->keys[0].build_type,
											 &integral) ||
			integral < filter->bitmap_lower || integral > filter->bitmap_upper)
			return;
		offset = (uint64) integral - (uint64) filter->bitmap_lower;
		filter->words[offset >> 6] |= UINT64CONST(1) << (offset & 63);
	}
	filter->inserted++;
}

bool
bloompg_filter_contains_slot(BloomPGFilter *filter,
							  TupleTableSlot *slot,
							  const AttrNumber *attributes)
{
	bool result;

	filter->lookups++;
	if (!filter->valid)
		return true;
	if (filter->kind == BLOOMPG_FILTER_BLOOM)
	{
		uint64 hash;

		result = bloompg_hash_slot(filter, slot, attributes, false, &hash) &&
			bloompg_bloom_contains_hash(filter, hash);
	}
	else
	{
		bool is_null;
		Datum value = slot_getattr(slot, attributes[0], &is_null);
		int64 integral;
		uint64 offset;

		result = !is_null &&
			bloompg_integral_datum(value, filter->keys[0].probe_type, &integral) &&
			integral >= filter->bitmap_lower && integral <= filter->bitmap_upper;
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

void
bloompg_filter_insert_vector(BloomPGFilter *filter,
	const BloomPGColumnStoreChunkView *view, uint32 row,
	const AttrNumber *attributes)
{
	if (filter->kind == BLOOMPG_FILTER_BLOOM)
	{
		uint64 hash;

		if (!bloompg_hash_vector(filter, view, row,
			attributes, true, &hash))
			return;
		bloompg_bloom_insert_hash(filter, hash);
	}
	else
	{
		Datum value;
		int64 integral;
		uint64 bit_offset;

		if (!AttributeNumberIsValid(attributes[0]))
			elog(ERROR, "invalid bloompg column-store bitmap attribute");
		if (!bloompg_column_chunk_get_datum(view, attributes[0], row, &value) ||
			!bloompg_integral_datum(value, filter->keys[0].build_type,
				&integral) ||
			integral < filter->bitmap_lower || integral > filter->bitmap_upper)
			return;
		bit_offset = (uint64) integral - (uint64) filter->bitmap_lower;
		filter->words[bit_offset >> 6] |= UINT64CONST(1) << (bit_offset & 63);
	}
	filter->inserted++;
}

bool
bloompg_filter_contains_vector(BloomPGFilter *filter,
	const BloomPGColumnStoreChunkView *view, uint32 row,
	const AttrNumber *attributes)
{
	bool result;

	filter->lookups++;
	if (!filter->valid)
		return true;
	if (filter->kind == BLOOMPG_FILTER_BLOOM)
	{
		uint64 hash;

		result = bloompg_hash_vector(filter, view, row,
			attributes, false, &hash) &&
			bloompg_bloom_contains_hash(filter, hash);
	}
	else
	{
		Datum value;
		int64 integral;
		uint64 bit_offset;

		if (!AttributeNumberIsValid(attributes[0]))
			elog(ERROR, "invalid bloompg column-store bitmap attribute");
		result = bloompg_column_chunk_get_datum(view, attributes[0], row,
			&value) &&
			bloompg_integral_datum(value, filter->keys[0].probe_type,
				&integral) &&
			integral >= filter->bitmap_lower && integral <= filter->bitmap_upper;
		if (result)
		{
			bit_offset = (uint64) integral - (uint64) filter->bitmap_lower;
			result = (filter->words[bit_offset >> 6] &
				(UINT64CONST(1) << (bit_offset & 63))) != 0;
		}
	}
	if (!result)
		filter->rejected++;
	return result;
}

/* Apply one filter to a whole selection vector.  Bitmap filters use their
 * native fixed-width column directly; composite and hashed filters retain the
 * general row helper.  Input and output may alias. */
uint32
bloompg_filter_select_vector(BloomPGFilter *filter,
	const BloomPGColumnStoreChunkView *view, const AttrNumber *attributes,
	const uint16 *input, uint32 input_count, uint16 *output)
{
	AttrNumber attribute;
	const BloomPGColumnVector *vector;
	Oid type_oid;
	uint32 input_index;
	uint32 output_count = 0;

	Assert(filter != NULL);
	if (filter->kind != BLOOMPG_FILTER_BITMAP || filter->nkeys != 1)
	{
		for (input_index = 0; input_index < input_count; input_index++)
		{
			uint32 row = input[input_index];

			if (bloompg_filter_contains_vector(filter, view, row, attributes))
				output[output_count++] = (uint16) row;
		}
		return output_count;
	}
	filter->lookups += input_count;
	if (!filter->valid)
	{
		if (input != output && input_count > 0)
			memcpy(output, input, mul_size(input_count, sizeof(uint16)));
		return input_count;
	}
	attribute = attributes[0];
	if (!AttributeNumberIsValid(attribute) ||
		(uint32) attribute > view->vector_count)
		elog(ERROR, "invalid bloompg bitmap vector attribute");
	vector = &view->vectors[attribute - 1];
	type_oid = filter->keys[0].probe_type;
	for (input_index = 0; input_index < input_count; input_index++)
	{
		uint32 row = input[input_index];
		int64 integral;
		uint64 offset;
		bool accepted = false;

		if (row >= view->count)
			elog(ERROR, "invalid bloompg vector selection");
		if (!bloompg_column_vector_is_valid(vector, row))
			goto rejected;
		switch (type_oid)
		{
			case INT2OID:
				if (vector->physical_type != BLOOMPG_VECTOR_BYVAL_2)
					elog(ERROR, "invalid bloompg int2 probe vector");
				integral = (int64) (int16)
					((const uint16 *) vector->data)[row];
				break;
			case INT4OID:
			case DATEOID:
				if (vector->physical_type != BLOOMPG_VECTOR_BYVAL_4)
					elog(ERROR, "invalid bloompg int4 probe vector");
				integral = (int64) (int32)
					((const uint32 *) vector->data)[row];
				break;
			case INT8OID:
				if (vector->physical_type != BLOOMPG_VECTOR_BYVAL_8)
					elog(ERROR, "invalid bloompg int8 probe vector");
				integral = (int64) ((const uint64 *) vector->data)[row];
				break;
			case OIDOID:
				if (vector->physical_type != BLOOMPG_VECTOR_BYVAL_4)
					elog(ERROR, "invalid bloompg oid probe vector");
				integral = (int64) ((const uint32 *) vector->data)[row];
				break;
			default:
				elog(ERROR, "invalid bloompg bitmap probe type %u", type_oid);
		}
		if (integral >= filter->bitmap_lower &&
			integral <= filter->bitmap_upper)
		{
			offset = (uint64) integral - (uint64) filter->bitmap_lower;
			accepted = (filter->words[offset >> 6] &
				(UINT64CONST(1) << (offset & 63))) != 0;
		}
		if (accepted)
		{
			output[output_count++] = (uint16) row;
			continue;
		}
rejected:
		filter->rejected++;
	}
	return output_count;
}

void
bloompg_filter_set_valid(BloomPGFilter *filter)
{
	filter->valid = true;
}

BloomPGFilter *
bloompg_filter_retain(BloomPGFilter *filter)
{
	if (filter == NULL || filter->refcount == UINT32_MAX)
		elog(ERROR, "invalid bloompg filter reference count");
	filter->refcount++;
	return filter;
}

void
bloompg_filter_free(BloomPGFilter *filter)
{
	MemoryContext context;

	if (filter == NULL)
		return;
	Assert(filter->refcount > 0);
	filter->refcount--;
	if (filter->refcount > 0)
		return;
	context = filter->memory_context;
	if (context != NULL)
		MemoryContextDelete(context);
}

bool
bloompg_filter_exact_statistics(const BloomPGFilter *filter, uint64 rows,
	uint64 *nonnull_rows, uint64 *null_rows, double *ndistinct)
{
	uint64 distinct = 0;
	uint64 word_count;
	uint64 word;

	if (filter == NULL || filter->kind != BLOOMPG_FILTER_BITMAP)
		return false;
	word_count = filter->nbits / 64;
	for (word = 0; word < word_count; word++)
	{
#if defined(__GNUC__) || defined(__clang__)
		distinct += (uint64) __builtin_popcountll(filter->words[word]);
#else
		uint64 value = filter->words[word];

		while (value != 0)
		{
			value &= value - 1;
			distinct++;
		}
#endif
	}
	*nonnull_rows = Min(filter->inserted, rows);
	*null_rows = rows - *nonnull_rows;
	*ndistinct = (double) Min(distinct, *nonnull_rows);
	return true;
}
