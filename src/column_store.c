#include "postgres.h"

#include "bloompg/column_store.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"

typedef struct BloomPGColumnChunk
{
	BloomPGColumnVector *vectors;
	uint16	   *selection;
	uint16		count;
	uint16		selected_count;
	uint64		selected_base;
} BloomPGColumnChunk;

/* One transfer invocation can retain several materializations at once.  The
 * budget is therefore shared by every store produced for that query, rather
 * than being copied into each relation.  DSM reservations participate in the
 * same counter because a parallel P1 path holds both representations. */
struct BloomPGMemoryBudget
{
	MemoryContext memory_context;
	Size		limit;
	Size		used;
	Size		dsm_reserved;
	uint32		references;
};

struct BloomPGColumnStore
{
	MemoryContext memory_context;
	MemoryContext column_context;
	BloomPGMemoryBudget *memory_budget;
	TupleDesc	descriptor;
	int			natts;
	BloomPGColumnChunk **chunks;
	uint32		nchunks;
	uint32		chunk_capacity;
	uint32		peak_chunks;
	uint64		rows;
	uint64		live_rows;
	bool		selection_bases_dirty;

	Size		memory_bytes;
	Size		peak_memory_bytes;

	uint64		varlen_values;
	Size		varlen_bytes;
	uint64		selection_rewrites;
	uint64		selection_rows_examined;
	uint64		selection_rows_rejected;
	uint64		slot_reconstructions;
};

static void
bloompg_memory_budget_exceeded(const BloomPGMemoryBudget *budget, Size bytes)
{
	ereport(ERROR,
		(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
		 errmsg("bloompg query materialization memory budget was exhausted"),
		 errdetail("The query has %zu bytes charged and requested %zu more bytes from a %zu-byte budget.",
			budget->used, bytes, budget->limit),
		 errhint("Increase bloompg.materialization_memory or let PostgreSQL use the native plan.")));
}

static void
bloompg_memory_budget_account(BloomPGMemoryBudget *budget, Size bytes)
{
	if (bytes == 0)
		return;
	if (bytes > bloompg_memory_budget_remaining(budget))
		bloompg_memory_budget_exceeded(budget, bytes);
	budget->used += bytes;
}

static void
bloompg_memory_budget_unaccount(BloomPGMemoryBudget *budget, Size bytes)
{
	if (bytes > budget->used)
		elog(ERROR, "bloompg materialization memory accounting underflow");
	budget->used -= bytes;
}

BloomPGMemoryBudget *
bloompg_memory_budget_create(MemoryContext parent, Size memory_limit)
{
	BloomPGMemoryBudget *budget;
	MemoryContext context;
	MemoryContext old_context;

	Assert(parent != NULL);
	context = AllocSetContextCreate(parent, "BloomPG query memory budget",
		ALLOCSET_SMALL_SIZES);
	old_context = MemoryContextSwitchTo(context);
	budget = palloc0(sizeof(BloomPGMemoryBudget));
	budget->memory_context = context;
	budget->limit = Max(memory_limit, (Size) 64 * 1024);
	budget->references = 1;
	MemoryContextSwitchTo(old_context);
	return budget;
}

void
bloompg_memory_budget_retain(BloomPGMemoryBudget *budget)
{
	Assert(budget != NULL && budget->references > 0);
	if (budget->references == UINT32_MAX)
		elog(ERROR, "too many bloompg materialization stores");
	budget->references++;
}

void
bloompg_memory_budget_release(BloomPGMemoryBudget *budget)
{
	MemoryContext context;

	if (budget == NULL)
		return;
	Assert(budget->references > 0);
	budget->references--;
	if (budget->references != 0)
		return;
	/* DSM itself is owned by PostgreSQL's ParallelContext.  This counter is a
	 * lifetime reservation, not an allocation in our MemoryContext, and the
	 * last materialization disappears only after every such scan has ended. */
	if (budget->dsm_reserved > 0)
	{
		bloompg_memory_budget_unaccount(budget, budget->dsm_reserved);
		budget->dsm_reserved = 0;
	}
	if (budget->used != 0)
		elog(ERROR, "bloompg query memory budget released with %zu bytes charged",
			budget->used);
	context = budget->memory_context;
	MemoryContextDelete(context);
}

Size
bloompg_memory_budget_limit(const BloomPGMemoryBudget *budget)
{
	return budget->limit;
}

Size
bloompg_memory_budget_remaining(const BloomPGMemoryBudget *budget)
{
	return budget->used < budget->limit ? budget->limit - budget->used : 0;
}

bool
bloompg_memory_budget_reserve(BloomPGMemoryBudget *budget, Size bytes)
{
	if (bytes > bloompg_memory_budget_remaining(budget))
		return false;
	bloompg_memory_budget_account(budget, bytes);
	return true;
}

void
bloompg_memory_budget_release_reservation(BloomPGMemoryBudget *budget,
	Size bytes)
{
	bloompg_memory_budget_unaccount(budget, bytes);
}

bool
bloompg_memory_budget_reserve_dsm(BloomPGMemoryBudget *budget, Size bytes)
{
	Size new_reserved;

	if (bytes == 0)
		return true;
	if (bytes > bloompg_memory_budget_remaining(budget))
		return false;
	new_reserved = add_size(budget->dsm_reserved, bytes);
	bloompg_memory_budget_account(budget, bytes);
	budget->dsm_reserved = new_reserved;
	return true;
}

static void
bloompg_column_store_account(BloomPGColumnStore *store, Size bytes)
{
	Size new_memory_bytes = add_size(store->memory_bytes, bytes);

	bloompg_memory_budget_account(store->memory_budget, bytes);
	store->memory_bytes = new_memory_bytes;
	store->peak_memory_bytes = Max(store->peak_memory_bytes,
		store->memory_bytes);
}

static void
bloompg_column_store_unaccount(BloomPGColumnStore *store, Size bytes)
{
	if (bytes > store->memory_bytes)
		elog(ERROR, "bloompg column-store memory accounting underflow");
	store->memory_bytes -= bytes;
	bloompg_memory_budget_unaccount(store->memory_budget, bytes);
}

/* Read the length of one packet/DSM datum without touching bytes outside its
 * declared heap.  In particular, VARSIZE_ANY and strlen are unsafe until the
 * corresponding header or terminator has been proven present. */
bool
bloompg_byref_datum_size(const char *heap, uint32 heap_used, int16 typlen,
	uint32 offset, Size *size)
{
	Size remaining;
	Size value_size;

	if (heap == NULL || size == NULL || offset >= heap_used)
		return false;
	remaining = (Size) heap_used - offset;
	if (typlen == -1)
	{
		const char *datum = heap + offset;

		if (VARATT_IS_1B(datum))
		{
			if (VARATT_IS_1B_E(datum))
				return false;
			value_size = VARSIZE_1B(datum);
			if (value_size < VARHDRSZ_SHORT)
				return false;
		}
		else
		{
			uint32 header;

			if (remaining < VARHDRSZ)
				return false;
			memcpy(&header, datum, sizeof(header));
			value_size = VARSIZE_4B(&header);
			if (value_size < VARHDRSZ)
				return false;
		}
	}
	else if (typlen == -2)
	{
		const char *terminator = memchr(heap + offset, '\0', remaining);

		if (terminator == NULL)
			return false;
		value_size = (Size) (terminator - (heap + offset)) + 1;
	}
	else if (typlen > 0)
		value_size = (Size) typlen;
	else
		return false;
	if (value_size > remaining)
		return false;
	*size = value_size;
	return true;
}

static Size
bloompg_column_vector_width(BloomPGVectorPhysicalType physical_type)
{
	switch (physical_type)
	{
		case BLOOMPG_VECTOR_BYVAL_1:
			return sizeof(uint8);
		case BLOOMPG_VECTOR_BYVAL_2:
			return sizeof(uint16);
		case BLOOMPG_VECTOR_BYVAL_4:
			return sizeof(uint32);
		case BLOOMPG_VECTOR_BYVAL_8:
			return sizeof(uint64);
		case BLOOMPG_VECTOR_BYREF:
			return sizeof(uint32);
	}
	pg_unreachable();
}

static BloomPGVectorPhysicalType
bloompg_column_vector_physical_type(Form_pg_attribute attribute)
{
	if (!attribute->attbyval)
		return BLOOMPG_VECTOR_BYREF;
	switch (attribute->attlen)
	{
		case 1:
			return BLOOMPG_VECTOR_BYVAL_1;
		case 2:
			return BLOOMPG_VECTOR_BYVAL_2;
		case 4:
			return BLOOMPG_VECTOR_BYVAL_4;
		case 8:
			return BLOOMPG_VECTOR_BYVAL_8;
		default:
			elog(ERROR, "unsupported bloompg by-value width %d",
				 attribute->attlen);
	}
	pg_unreachable();
}

static uint32
bloompg_column_vector_heap_capacity(uint32 old_capacity, Size required)
{
	uint32 new_capacity;
	uint32 limit;

	if (required <= old_capacity)
		return old_capacity;
	if (required > UINT32_MAX || required > MaxAllocSize)
		elog(ERROR, "bloompg vector heap exceeds PostgreSQL allocation limits");
	new_capacity = old_capacity == 0 ? 4096 : old_capacity;
	limit = (uint32) Min((Size) UINT32_MAX, (Size) MaxAllocSize);
	while ((Size) new_capacity < required)
	{
		if (new_capacity > limit / 2)
		{
			new_capacity = (uint32) required;
			break;
		}
		new_capacity *= 2;
	}
	return new_capacity;
}

static void
bloompg_column_store_require_growth(const BloomPGColumnStore *store,
	Size additional)
{
	if (additional > bloompg_memory_budget_remaining(store->memory_budget))
		bloompg_memory_budget_exceeded(store->memory_budget, additional);
}

static Size
bloompg_column_store_new_chunk_bytes(const BloomPGColumnStore *store)
{
	Size bytes = sizeof(BloomPGColumnChunk);
	Size validity_bytes = sizeof(uint64) * BLOOMPG_VALIDITY_WORDS;
	int attribute;

	if (store->nchunks == store->chunk_capacity)
	{
		uint32 old_capacity = store->chunk_capacity;
		uint32 new_capacity = old_capacity == 0 ? 16 : old_capacity * 2;

		if (new_capacity <= old_capacity ||
			new_capacity > (uint32) (MaxAllocSize /
				sizeof(BloomPGColumnChunk *)))
			elog(ERROR, "bloompg materialization has too many vector chunks");
		bytes = add_size(bytes,
			mul_size(new_capacity - old_capacity,
				sizeof(BloomPGColumnChunk *)));
	}
	bytes = add_size(bytes,
		mul_size(sizeof(BloomPGColumnVector), store->natts));
	bytes = add_size(bytes,
		mul_size(sizeof(uint16), BLOOMPG_VECTOR_SIZE));
	for (attribute = 0; attribute < store->natts; attribute++)
	{
		Form_pg_attribute form = TupleDescAttr(store->descriptor, attribute);
		BloomPGVectorPhysicalType physical_type =
			bloompg_column_vector_physical_type(form);

		bytes = add_size(bytes, mul_size(
			bloompg_column_vector_width(physical_type), BLOOMPG_VECTOR_SIZE));
		bytes = add_size(bytes, validity_bytes);
	}
	return bytes;
}

static Size
bloompg_column_store_slot_growth(BloomPGColumnStore *store,
	TupleTableSlot *slot, bool new_chunk)
{
	BloomPGColumnChunk *chunk = new_chunk ? NULL :
		store->chunks[store->nchunks - 1];
	Size bytes = new_chunk ? bloompg_column_store_new_chunk_bytes(store) : 0;
	int attribute;

	for (attribute = 0; attribute < store->natts; attribute++)
	{
		Form_pg_attribute form = TupleDescAttr(store->descriptor, attribute);
		BloomPGColumnVector *vector;
		Size value_bytes;
		Size required;
		uint32 old_capacity;
		uint32 new_capacity;
		uint32 heap_used;

		if (slot->tts_isnull[attribute] || form->attbyval)
			continue;
		vector = chunk == NULL ? NULL : &chunk->vectors[attribute];
		heap_used = vector == NULL ? 0 : vector->heap_used;
		old_capacity = vector == NULL ? 0 : vector->heap_capacity;
		if (form->attlen == -1)
			value_bytes = toast_raw_datum_size(slot->tts_values[attribute]);
		else if (form->attlen == -2)
			value_bytes = strlen(DatumGetCString(slot->tts_values[attribute])) + 1;
		else
		{
			if (form->attlen <= 0)
				elog(ERROR, "invalid bloompg by-reference width %d", form->attlen);
			value_bytes = (Size) form->attlen;
		}
		required = add_size(MAXALIGN(heap_used), value_bytes);
		new_capacity = bloompg_column_vector_heap_capacity(old_capacity,
			required);
		bytes = add_size(bytes, (Size) new_capacity - old_capacity);
	}
	return bytes;
}

static Size
bloompg_column_store_dense_growth(BloomPGColumnStore *store,
	const BloomPGColumnVector *vectors, uint32 source_row, uint32 count,
	bool new_chunk)
{
	BloomPGColumnChunk *chunk = new_chunk ? NULL :
		store->chunks[store->nchunks - 1];
	Size bytes = new_chunk ? bloompg_column_store_new_chunk_bytes(store) : 0;
	int attribute;

	for (attribute = 0; attribute < store->natts; attribute++)
	{
		const BloomPGColumnVector *source = &vectors[attribute];
		BloomPGColumnVector *destination = chunk == NULL ? NULL :
			&chunk->vectors[attribute];
		uint32 heap_used;
		uint32 old_capacity;
		uint32 new_capacity;
		Size required;
		uint32 first_offset = UINT32_MAX;
		uint32 last_end = 0;
		uint32 row;

		if (source->physical_type != BLOOMPG_VECTOR_BYREF ||
			source->heap_used == 0)
			continue;
		for (row = 0; row < count; row++)
		{
			uint32 input_row = source_row + row;
			uint32 offset;
			Size value_size;

			if (!bloompg_column_vector_is_valid(source, input_row))
				continue;
			offset = ((const uint32 *) source->data)[input_row];
			if (!bloompg_byref_datum_size(source->heap, source->heap_used,
					source->typlen, offset, &value_size))
				elog(ERROR, "invalid bloompg dense vector value");
			first_offset = Min(first_offset, offset);
			last_end = Max(last_end, offset + (uint32) value_size);
		}
		if (first_offset == UINT32_MAX)
			continue;
		heap_used = destination == NULL ? 0 : destination->heap_used;
		old_capacity = destination == NULL ? 0 : destination->heap_capacity;
		required = add_size(MAXALIGN(heap_used),
			(Size) last_end - first_offset);
		new_capacity = bloompg_column_vector_heap_capacity(old_capacity,
			required);
		bytes = add_size(bytes, (Size) new_capacity - old_capacity);
	}
	return bytes;
}

static void
bloompg_column_store_ensure_chunk_array(BloomPGColumnStore *store)
{
	uint32		old_capacity;
	uint32		new_capacity;
	Size		old_bytes;
	Size		new_bytes;

	if (store->nchunks < store->chunk_capacity)
		return;
	old_capacity = store->chunk_capacity;
	new_capacity = old_capacity == 0 ? 16 : old_capacity * 2;
	if (new_capacity <= old_capacity ||
		new_capacity > (uint32) (MaxAllocSize /
			sizeof(BloomPGColumnChunk *)))
		elog(ERROR, "bloompg materialization has too many vector chunks");
	old_bytes = (Size) old_capacity * sizeof(BloomPGColumnChunk *);
	new_bytes = (Size) new_capacity * sizeof(BloomPGColumnChunk *);
	if (store->chunks == NULL)
		store->chunks = MemoryContextAlloc(store->memory_context, new_bytes);
	else
		store->chunks = repalloc(store->chunks, new_bytes);
	MemSet(store->chunks + old_capacity, 0, new_bytes - old_bytes);
	store->chunk_capacity = new_capacity;
	bloompg_column_store_account(store, new_bytes - old_bytes);
}

static void
bloompg_column_vector_initialize(BloomPGColumnStore *store,
	BloomPGColumnVector *vector, Form_pg_attribute attribute)
{
	Size		width;
	Size		data_bytes;
	Size		validity_bytes = sizeof(uint64) * BLOOMPG_VALIDITY_WORDS;

	vector->type_oid = attribute->atttypid;
	vector->typlen = attribute->attlen;
	vector->physical_type = bloompg_column_vector_physical_type(attribute);
	width = bloompg_column_vector_width(vector->physical_type);
	data_bytes = mul_size(width, BLOOMPG_VECTOR_SIZE);
	vector->data = palloc0(data_bytes);
	vector->validity = palloc0(validity_bytes);
	bloompg_column_store_account(store, add_size(data_bytes, validity_bytes));
}

static BloomPGColumnChunk *
bloompg_column_store_new_chunk(BloomPGColumnStore *store)
{
	BloomPGColumnChunk *chunk;
	MemoryContext old_context;
	int			attribute;

	if (store->descriptor == NULL || store->natts <= 0)
		elog(ERROR, "invalid bloompg vector descriptor");
	bloompg_column_store_ensure_chunk_array(store);
	old_context = MemoryContextSwitchTo(store->column_context);
	chunk = palloc0(sizeof(BloomPGColumnChunk));
	chunk->vectors = palloc0_array(BloomPGColumnVector, store->natts);
	chunk->selection = palloc_array(uint16, BLOOMPG_VECTOR_SIZE);
	bloompg_column_store_account(store, sizeof(BloomPGColumnChunk));
	bloompg_column_store_account(store,
		mul_size(sizeof(BloomPGColumnVector), store->natts));
	bloompg_column_store_account(store,
		mul_size(sizeof(uint16), BLOOMPG_VECTOR_SIZE));
	for (attribute = 0; attribute < store->natts; attribute++)
		bloompg_column_vector_initialize(store, &chunk->vectors[attribute],
			TupleDescAttr(store->descriptor, attribute));
	MemoryContextSwitchTo(old_context);
	store->chunks[store->nchunks++] = chunk;
	store->peak_chunks = Max(store->peak_chunks, store->nchunks);
	store->selection_bases_dirty = true;
	return chunk;
}

static void
bloompg_column_vector_reserve_heap(BloomPGColumnStore *store,
	BloomPGColumnVector *vector, Size required)
{
	uint32		old_capacity = vector->heap_capacity;
	uint32		new_capacity;

	if (required <= old_capacity)
		return;
	new_capacity = bloompg_column_vector_heap_capacity(old_capacity, required);
	if (vector->heap == NULL)
		vector->heap = palloc(new_capacity);
	else
		vector->heap = repalloc(vector->heap, new_capacity);
	vector->heap_capacity = new_capacity;
	bloompg_column_store_account(store, (Size) new_capacity - old_capacity);
}

static void
bloompg_column_vector_append(BloomPGColumnStore *store,
	BloomPGColumnVector *vector, uint32 row, Datum value, bool is_null)
{
	if (is_null)
		return;
	vector->validity[row / BLOOMPG_VALIDITY_BITS] |=
		UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS);
	switch (vector->physical_type)
	{
		case BLOOMPG_VECTOR_BYVAL_1:
			((uint8 *) vector->data)[row] = (uint8) value;
			break;
		case BLOOMPG_VECTOR_BYVAL_2:
			((uint16 *) vector->data)[row] = (uint16) value;
			break;
		case BLOOMPG_VECTOR_BYVAL_4:
			((uint32 *) vector->data)[row] = (uint32) value;
			break;
		case BLOOMPG_VECTOR_BYVAL_8:
			((uint64 *) vector->data)[row] = (uint64) value;
			break;
		case BLOOMPG_VECTOR_BYREF:
		{
			const char *source;
			struct varlena *detoasted = NULL;
			Size		size;
			Size		offset = MAXALIGN(vector->heap_used);
			Size		required;

			if (vector->typlen == -1)
			{
				detoasted = PG_DETOAST_DATUM_PACKED(value);
				source = (const char *) detoasted;
				size = VARSIZE_ANY(detoasted);
			}
			else if (vector->typlen == -2)
			{
				source = DatumGetCString(value);
				size = strlen(source) + 1;
			}
			else
			{
				Assert(vector->typlen > 0);
				source = (const char *) DatumGetPointer(value);
				size = (Size) vector->typlen;
			}
			required = add_size(offset, size);
			bloompg_column_vector_reserve_heap(store, vector, required);
			memcpy(vector->heap + offset, source, size);
			((uint32 *) vector->data)[row] = (uint32) offset;
			vector->heap_used = (uint32) required;
			store->varlen_values++;
			store->varlen_bytes = add_size(store->varlen_bytes, size);
			if (detoasted != NULL &&
				(Pointer) detoasted != DatumGetPointer(value))
				pfree(detoasted);
			break;
		}
	}
}

/* Copy an appended validity range a machine word at a time.  Dense packets
 * and destination chunks are normally both vector-aligned, making this one
 * memcpy; the shifted path handles final packets from interleaved workers. */
static void
bloompg_column_vector_copy_validity(BloomPGColumnVector *destination,
	uint32 destination_row, const BloomPGColumnVector *source,
	uint32 source_row, uint32 count)
{
	if ((destination_row % BLOOMPG_VALIDITY_BITS) == 0 &&
		(source_row % BLOOMPG_VALIDITY_BITS) == 0)
	{
		uint32 words = count / BLOOMPG_VALIDITY_BITS;
		uint32 tail = count % BLOOMPG_VALIDITY_BITS;

		if (words > 0)
			memcpy(destination->validity +
				(destination_row / BLOOMPG_VALIDITY_BITS),
				source->validity + (source_row / BLOOMPG_VALIDITY_BITS),
				mul_size(words, sizeof(uint64)));
		if (tail > 0)
		{
			uint64 mask = (UINT64CONST(1) << tail) - 1;

			destination->validity[
				(destination_row / BLOOMPG_VALIDITY_BITS) + words] |=
				source->validity[
					(source_row / BLOOMPG_VALIDITY_BITS) + words] & mask;
		}
		return;
	}
	while (count > 0)
	{
		uint32 destination_bit = destination_row % BLOOMPG_VALIDITY_BITS;
		uint32 source_bit = source_row % BLOOMPG_VALIDITY_BITS;
		uint32 take = Min(count, BLOOMPG_VALIDITY_BITS - destination_bit);
		uint64 bits = source->validity[source_row / BLOOMPG_VALIDITY_BITS] >>
			source_bit;
		uint64 mask;

		if (source_bit + take > BLOOMPG_VALIDITY_BITS)
			bits |= source->validity[
				(source_row / BLOOMPG_VALIDITY_BITS) + 1] <<
				(BLOOMPG_VALIDITY_BITS - source_bit);
		mask = take == BLOOMPG_VALIDITY_BITS ? UINT64_MAX :
			(UINT64CONST(1) << take) - 1;
		destination->validity[
			destination_row / BLOOMPG_VALIDITY_BITS] |=
			(bits & mask) << destination_bit;
		destination_row += take;
		source_row += take;
		count -= take;
	}
}

/* A packed worker vector already owns one aligned, row-ordered heap.  Import
 * a slice as one byte range and relocate its 32-bit offsets, avoiding a
 * detoast check and memcpy for every individual value in the leader. */
static void
bloompg_column_vector_append_dense_byref(BloomPGColumnStore *store,
	BloomPGColumnVector *destination, uint32 destination_row,
	const BloomPGColumnVector *source, uint32 source_row, uint32 count)
{
	uint32 first_offset = UINT32_MAX;
	uint32 last_end = 0;
	uint32 values = 0;
	Size value_bytes = 0;
	uint32 row;

	Assert(destination->physical_type == BLOOMPG_VECTOR_BYREF);
	Assert(source->physical_type == BLOOMPG_VECTOR_BYREF);
	for (row = 0; row < count; row++)
	{
		uint32 input_row = source_row + row;
		uint32 offset;
		Size size;

		if (!bloompg_column_vector_is_valid(source, input_row))
			continue;
		offset = ((const uint32 *) source->data)[input_row];
		if (offset >= source->heap_used)
			elog(ERROR, "invalid bloompg packed vector offset");
		if (!bloompg_byref_datum_size(source->heap, source->heap_used,
			source->typlen, offset, &size))
			elog(ERROR, "invalid bloompg packed vector value");
		first_offset = Min(first_offset, offset);
		last_end = Max(last_end, offset + (uint32) size);
		value_bytes = add_size(value_bytes, size);
		values++;
	}
	if (values > 0)
	{
		Size destination_offset = MAXALIGN(destination->heap_used);
		Size copy_bytes = (Size) last_end - first_offset;
		Size required = add_size(destination_offset, copy_bytes);

		bloompg_column_vector_reserve_heap(store, destination, required);
		memcpy(destination->heap + destination_offset,
			source->heap + first_offset, copy_bytes);
		memcpy((uint32 *) destination->data + destination_row,
			(const uint32 *) source->data + source_row,
			mul_size(count, sizeof(uint32)));
		for (row = 0; row < count; row++)
		{
			uint32 input_row = source_row + row;

			if (bloompg_column_vector_is_valid(source, input_row))
				((uint32 *) destination->data)[destination_row + row] =
					(uint32) destination_offset +
					((const uint32 *) source->data)[input_row] - first_offset;
		}
		destination->heap_used = (uint32) required;
		store->varlen_values += values;
		store->varlen_bytes = add_size(store->varlen_bytes, value_bytes);
	}
	bloompg_column_vector_copy_validity(destination, destination_row,
		source, source_row, count);
}

static void
bloompg_column_store_fill_memory_slot(BloomPGColumnStore *store,
	uint32 chunk_index, uint32 row, TupleTableSlot *slot)
{
	BloomPGColumnChunk *chunk;
	int			attribute;

	Assert(chunk_index < store->nchunks && row < BLOOMPG_VECTOR_SIZE);
	chunk = store->chunks[chunk_index];
	Assert(row < chunk->count);
	ExecClearTuple(slot);
	for (attribute = 0; attribute < store->natts; attribute++)
	{
		BloomPGColumnVector *vector = &chunk->vectors[attribute];

		slot->tts_isnull[attribute] =
			!bloompg_column_vector_is_valid(vector, row);
		if (!slot->tts_isnull[attribute])
			slot->tts_values[attribute] =
				bloompg_column_vector_get_datum(vector, row);
		else
			slot->tts_values[attribute] = (Datum) 0;
	}
	ExecStoreVirtualTuple(slot);
	store->slot_reconstructions++;
}

static void
bloompg_column_store_rebuild_selected_bases(BloomPGColumnStore *store)
{
	uint64		base = 0;
	uint32		chunk_index;

	if (!store->selection_bases_dirty)
		return;
	for (chunk_index = 0; chunk_index < store->nchunks; chunk_index++)
	{
		BloomPGColumnChunk *chunk = store->chunks[chunk_index];

		chunk->selected_base = base;
		base += chunk->selected_count;
	}
	if (base != store->live_rows)
		elog(ERROR, "bloompg vector selection cardinality changed");
	store->selection_bases_dirty = false;
}

static BloomPGColumnChunk *
bloompg_column_store_find_selected(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan, uint64 logical_row, uint32 *chunk_index,
	uint32 *physical_row)
{
	uint32		low = 0;
	uint32		high = store->nchunks;
	BloomPGColumnChunk *chunk;

	bloompg_column_store_rebuild_selected_bases(store);
	if (scan->memory_chunk_valid && scan->memory_chunk < store->nchunks)
	{
		chunk = store->chunks[scan->memory_chunk];
		if (logical_row >= chunk->selected_base &&
			logical_row < chunk->selected_base + chunk->selected_count)
		{
			*chunk_index = scan->memory_chunk;
			*physical_row = chunk->selection[logical_row - chunk->selected_base];
			return chunk;
		}
	}
	while (low < high)
	{
		uint32 middle = low + (high - low) / 2;

		chunk = store->chunks[middle];
		if (logical_row < chunk->selected_base)
			high = middle;
		else if (logical_row >= chunk->selected_base + chunk->selected_count)
			low = middle + 1;
		else
		{
			scan->memory_chunk = middle;
			scan->memory_chunk_valid = true;
			*chunk_index = middle;
			*physical_row =
				chunk->selection[logical_row - chunk->selected_base];
			return chunk;
		}
	}
	elog(ERROR, "bloompg could not resolve selected vector row");
	pg_unreachable();
}

BloomPGColumnStore *
bloompg_column_store_create(MemoryContext parent,
	BloomPGMemoryBudget *memory_budget)
{
	BloomPGColumnStore *store;
	MemoryContext context;
	MemoryContext old_context;

	Assert(parent != NULL);
	Assert(memory_budget != NULL);
	context = AllocSetContextCreate(parent, "BloomPG vector materialization",
		ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(context);
	store = palloc0(sizeof(BloomPGColumnStore));
	store->memory_context = context;
	store->column_context = AllocSetContextCreate(context,
		"BloomPG vector chunks", ALLOCSET_DEFAULT_SIZES);
	store->memory_budget = memory_budget;
	bloompg_memory_budget_retain(memory_budget);
	MemoryContextSwitchTo(old_context);
	return store;
}

BloomPGMemoryBudget *
bloompg_column_store_memory_budget(const BloomPGColumnStore *store)
{
	return store->memory_budget;
}

void
bloompg_column_store_set_descriptor(BloomPGColumnStore *store,
	TupleDesc descriptor)
{
	Assert(store != NULL);
	Assert(descriptor != NULL);
	if (store->descriptor != NULL)
	{
		if (store->descriptor != descriptor)
			elog(ERROR, "bloompg materialization descriptor changed");
		return;
	}
	if (descriptor->natts <= 0)
		elog(ERROR, "bloompg materialization has no vectors");
	store->descriptor = descriptor;
	store->natts = descriptor->natts;
}

void
bloompg_column_store_append_slot(BloomPGColumnStore *store,
	TupleTableSlot *slot)
{
	BloomPGColumnChunk *chunk;
	uint32		row;
	MemoryContext old_context;
	int			attribute;

	Assert(store != NULL);
	Assert(store->descriptor != NULL);
	slot_getallattrs(slot);
	{
		bool new_chunk = store->nchunks == 0 ||
			store->chunks[store->nchunks - 1]->count == BLOOMPG_VECTOR_SIZE;
		Size growth = bloompg_column_store_slot_growth(store, slot, new_chunk);

		bloompg_column_store_require_growth(store, growth);
	}
	if (store->nchunks == 0 ||
		store->chunks[store->nchunks - 1]->count == BLOOMPG_VECTOR_SIZE)
		chunk = bloompg_column_store_new_chunk(store);
	else
		chunk = store->chunks[store->nchunks - 1];
	row = chunk->count;
	old_context = MemoryContextSwitchTo(store->column_context);
	for (attribute = 0; attribute < store->natts; attribute++)
		bloompg_column_vector_append(store, &chunk->vectors[attribute], row,
			slot->tts_values[attribute], slot->tts_isnull[attribute]);
	MemoryContextSwitchTo(old_context);
	chunk->selection[chunk->selected_count++] = (uint16) row;
	chunk->count++;
	store->rows++;
	store->live_rows++;
	store->selection_bases_dirty = true;
}

/* Import one dense vector packet.  Parallel transfer workers have already
 * deformed and columnized these rows, so the leader can copy fixed-width
 * columns in bulk instead of reconstructing one MinimalTuple at a time. */
void
bloompg_column_store_append_dense(BloomPGColumnStore *store,
	const BloomPGColumnVector *vectors, uint32 vector_count, uint32 count)
{
	uint32 source_row = 0;

	Assert(store != NULL && store->descriptor != NULL);
	if (vector_count != (uint32) store->natts || count == 0 ||
		count > BLOOMPG_VECTOR_SIZE)
		elog(ERROR, "invalid bloompg dense vector packet");
	while (source_row < count)
	{
		BloomPGColumnChunk *chunk;
		uint32 destination_row;
		uint32 take;
		uint32 row;
		int attribute;

		{
			bool new_chunk;
			Size growth;

			new_chunk = store->nchunks == 0 ||
				store->chunks[store->nchunks - 1]->count == BLOOMPG_VECTOR_SIZE;
			destination_row = new_chunk ? 0 :
				store->chunks[store->nchunks - 1]->count;
			take = Min(count - source_row,
				BLOOMPG_VECTOR_SIZE - destination_row);
			growth = bloompg_column_store_dense_growth(store, vectors,
				source_row, take, new_chunk);

			bloompg_column_store_require_growth(store, growth);
		}
		if (store->nchunks == 0 ||
			store->chunks[store->nchunks - 1]->count == BLOOMPG_VECTOR_SIZE)
			chunk = bloompg_column_store_new_chunk(store);
		else
			chunk = store->chunks[store->nchunks - 1];
		Assert(destination_row == chunk->count);
		for (attribute = 0; attribute < store->natts; attribute++)
		{
			BloomPGColumnVector *destination = &chunk->vectors[attribute];
			const BloomPGColumnVector *source = &vectors[attribute];
			Form_pg_attribute form = TupleDescAttr(store->descriptor, attribute);

			if (source->type_oid != form->atttypid ||
				source->typlen != form->attlen ||
				source->physical_type != destination->physical_type)
				elog(ERROR, "bloompg dense vector descriptor changed");
			if (destination->physical_type != BLOOMPG_VECTOR_BYREF)
			{
				Size width = bloompg_column_vector_width(
					destination->physical_type);

				memcpy((char *) destination->data +
					mul_size(destination_row, width),
					(const char *) source->data + mul_size(source_row, width),
					mul_size(take, width));
				bloompg_column_vector_copy_validity(destination,
					destination_row, source, source_row, take);
			}
			else
			{
				MemoryContext old_context = MemoryContextSwitchTo(
					store->column_context);

				bloompg_column_vector_append_dense_byref(store, destination,
					destination_row, source, source_row, take);
				MemoryContextSwitchTo(old_context);
			}
		}
		for (row = 0; row < take; row++)
			chunk->selection[chunk->selected_count++] =
				(uint16) (destination_row + row);
		chunk->count += (uint16) take;
		store->rows += take;
		store->live_rows += take;
		store->selection_bases_dirty = true;
		source_row += take;
	}
}

void
bloompg_column_store_begin_scan(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan)
{
	MemSet(scan, 0, sizeof(*scan));
	bloompg_column_store_rebuild_selected_bases(store);
}

void
bloompg_column_store_rescan(BloomPGColumnStoreScan *scan)
{
	scan->position = 0;
	scan->memory_chunk_valid = false;
}

bool
bloompg_column_store_next(BloomPGColumnStore *store,
	BloomPGColumnStoreScan *scan, bool forward, TupleTableSlot *slot,
	uint64 *row_index)
{
	uint64		logical_row;
	uint32		chunk_index;
	uint32		physical_row;

	if (forward)
	{
		if (scan->position >= store->live_rows)
			return false;
		logical_row = scan->position++;
	}
	else
	{
		if (scan->position == 0)
			return false;
		logical_row = --scan->position;
	}
	(void) bloompg_column_store_find_selected(store, scan, logical_row,
		&chunk_index, &physical_row);
	bloompg_column_store_fill_memory_slot(store, chunk_index, physical_row,
		slot);
	if (row_index != NULL)
		*row_index = (uint64) chunk_index * BLOOMPG_VECTOR_SIZE + physical_row;
	return true;
}

uint32
bloompg_column_store_chunk_count(const BloomPGColumnStore *store)
{
	return store->nchunks;
}

bool
bloompg_column_store_chunk_view(const BloomPGColumnStore *store,
	uint32 chunk_index, BloomPGColumnStoreChunkView *view)
{
	BloomPGColumnChunk *chunk;

	Assert(store != NULL);
	Assert(view != NULL);
	if (chunk_index >= store->nchunks)
		return false;
	chunk = store->chunks[chunk_index];
	view->row_base = (uint64) chunk_index * BLOOMPG_VECTOR_SIZE;
	view->count = chunk->count;
	view->selected_count = chunk->selected_count;
	view->vector_count = store->natts;
	view->selection = chunk->selection;
	view->vectors = chunk->vectors;
	return true;
}

void
bloompg_column_store_set_chunk_selection(BloomPGColumnStore *store,
	uint32 chunk_index, const uint16 *selection, uint32 selected_count)
{
	BloomPGColumnChunk *chunk;
	uint32		old_count;
	uint32		index;

	if (chunk_index >= store->nchunks)
		elog(ERROR, "invalid bloompg vector selection target");
	chunk = store->chunks[chunk_index];
	old_count = chunk->selected_count;
	if (selected_count > old_count)
		elog(ERROR, "bloompg vector selection grew during filtering");
	for (index = 0; index < selected_count; index++)
	{
		if (selection[index] >= chunk->count ||
			(index > 0 && selection[index] <= selection[index - 1]))
			elog(ERROR, "invalid bloompg vector selection");
	}
	if (selected_count > 0)
		memcpy(chunk->selection, selection,
			mul_size(selected_count, sizeof(uint16)));
	chunk->selected_count = (uint16) selected_count;
	store->live_rows -= old_count - selected_count;
	store->selection_rewrites++;
	store->selection_rows_examined += old_count;
	store->selection_rows_rejected += old_count - selected_count;
	store->selection_bases_dirty = true;
}

void
bloompg_column_store_deactivate(BloomPGColumnStore *store, uint64 row_index)
{
	if (row_index >= store->rows)
		elog(ERROR, "bloompg materialization row index is out of range");
	{
		uint64 chunk64 = row_index / BLOOMPG_VECTOR_SIZE;
		uint16 row = (uint16) (row_index % BLOOMPG_VECTOR_SIZE);
		BloomPGColumnChunk *chunk;
		uint32 index;

		if (chunk64 >= store->nchunks)
			return;
		chunk = store->chunks[chunk64];
		for (index = 0; index < chunk->selected_count; index++)
		{
			if (chunk->selection[index] != row)
				continue;
			if (index + 1 < chunk->selected_count)
				memmove(&chunk->selection[index], &chunk->selection[index + 1],
					(chunk->selected_count - index - 1) * sizeof(uint16));
			chunk->selected_count--;
			store->live_rows--;
			store->selection_rewrites++;
			store->selection_rows_examined++;
			store->selection_rows_rejected++;
			store->selection_bases_dirty = true;
			return;
		}
	}
}

void
bloompg_column_store_deactivate_all(BloomPGColumnStore *store)
{
	uint32 chunk_index;

	for (chunk_index = 0; chunk_index < store->nchunks; chunk_index++)
		store->chunks[chunk_index]->selected_count = 0;
	store->selection_bases_dirty = true;
	store->selection_rewrites++;
	store->selection_rows_examined += store->live_rows;
	store->selection_rows_rejected += store->live_rows;
	store->live_rows = 0;
}

uint64
bloompg_column_store_rows(const BloomPGColumnStore *store)
{
	return store->rows;
}

uint64
bloompg_column_store_live_rows(const BloomPGColumnStore *store)
{
	return store->live_rows;
}

Size
bloompg_column_store_memory_bytes(const BloomPGColumnStore *store)
{
	return store->memory_bytes;
}

Size
bloompg_column_store_peak_memory_bytes(const BloomPGColumnStore *store)
{
	return store->peak_memory_bytes;
}

Size
bloompg_column_store_memory_limit(const BloomPGColumnStore *store)
{
	return bloompg_memory_budget_limit(store->memory_budget);
}

uint32
bloompg_column_store_vector_chunks(const BloomPGColumnStore *store)
{
	return store->peak_chunks;
}

uint64
bloompg_column_store_varlen_values(const BloomPGColumnStore *store)
{
	return store->varlen_values;
}

Size
bloompg_column_store_varlen_bytes(const BloomPGColumnStore *store)
{
	return store->varlen_bytes;
}

uint64
bloompg_column_store_selection_rewrites(const BloomPGColumnStore *store)
{
	return store->selection_rewrites;
}

uint64
bloompg_column_store_selection_rows_examined(const BloomPGColumnStore *store)
{
	return store->selection_rows_examined;
}

uint64
bloompg_column_store_selection_rows_rejected(const BloomPGColumnStore *store)
{
	return store->selection_rows_rejected;
}

uint64
bloompg_column_store_slot_reconstructions(const BloomPGColumnStore *store)
{
	return store->slot_reconstructions;
}

void
bloompg_column_store_destroy(BloomPGColumnStore *store)
{
	MemoryContext context;
	BloomPGMemoryBudget *memory_budget;

	if (store == NULL)
		return;
	context = store->memory_context;
	memory_budget = store->memory_budget;
	if (store->memory_bytes > 0)
		bloompg_column_store_unaccount(store, store->memory_bytes);
	MemoryContextDelete(context);
	bloompg_memory_budget_release(memory_budget);
}
