#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/column_store.h"
#include "bloompg/scan.h"
#include "bloompg/scan_internal.h"

#include "catalog/pg_type_d.h"
#include "access/detoast.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "utils/memutils.h"

/* Parallel Gather normally serializes one MinimalTuple per row.  This scan
 * node moves deformation and columnization into each worker, then emits one
 * compact bytea per vector.  The leader validates the packet and imports its
 * fixed-width columns with bulk copies. */

#define BLOOMPG_PACK_MAGIC 0x42504756U
#define BLOOMPG_PACK_MAX_TARGET_BYTES ((Size) 4 * 1024 * 1024)
#define BLOOMPG_PACK_MIN_TARGET_BYTES ((Size) 64 * 1024)

typedef struct BloomPGPackedHeader
{
	uint32		magic;
	uint32		total_size;
	uint32		rows;
	uint32		natts;
} BloomPGPackedHeader;

typedef struct BloomPGPackedVector
{
	uint32		type_oid;
	int16		typlen;
	uint8		physical_type;
	uint8		reserved;
	uint32		data_offset;
	uint32		validity_offset;
	uint32		heap_offset;
	uint32		heap_used;
} BloomPGPackedVector;

typedef struct BloomPGPackState
{
	CustomScanState css;
	PlanState  *child;
	TupleDesc	descriptor;
	BloomPGColumnVector *vectors;
	bytea	   *payload;
	Size		payload_capacity;
	Size		packet_target;
	Size		packet_limit;
	uint64		packets;
	uint64		rows;
} BloomPGPackState;

static Node *bloompg_create_pack_state(CustomScan *scan);
static void bloompg_begin_pack(CustomScanState *node, EState *estate,
	int eflags);
static TupleTableSlot *bloompg_exec_pack(CustomScanState *node);
static void bloompg_end_pack(CustomScanState *node);
static void bloompg_rescan_pack(CustomScanState *node);
static void bloompg_explain_pack(CustomScanState *node, List *ancestors,
	ExplainState *explain);

const CustomScanMethods bloompg_pack_scan_methods = {
	.CustomName = "BloomPG Vector Pack",
	.CreateCustomScanState = bloompg_create_pack_state
};

static const CustomExecMethods bloompg_pack_exec_methods = {
	.CustomName = "BloomPG Vector Pack",
	.BeginCustomScan = bloompg_begin_pack,
	.ExecCustomScan = bloompg_exec_pack,
	.EndCustomScan = bloompg_end_pack,
	.ReScanCustomScan = bloompg_rescan_pack,
	.ExplainCustomScan = bloompg_explain_pack
};

static Size
bloompg_pack_vector_width(BloomPGVectorPhysicalType physical_type)
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
bloompg_pack_physical_type(Form_pg_attribute attribute)
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
			elog(ERROR, "unsupported bloompg packed by-value width %d",
				attribute->attlen);
	}
	pg_unreachable();
}

/* A worker retains input vectors, their by-reference heaps, and one serialized
 * payload at the same time.  Six copies per worker is the conservative live
 * transport bound.  Give transport at most half of the query budget so an
 * otherwise valid parallel scan cannot reserve all memory and crowd out the
 * column stores it is producing.  The normal 4MB target remains the upper
 * bound for cache locality. */
static Size
bloompg_pack_target_bytes(int parallel_workers)
{
	Size query_bytes = mul_size((Size) bloompg_materialization_memory_kb,
		(Size) 1024);
	Size workers = (Size) Max(parallel_workers, 1);
	Size target = query_bytes / workers / 12;

	return Min(BLOOMPG_PACK_MAX_TARGET_BYTES,
		Max(target, BLOOMPG_PACK_MIN_TARGET_BYTES));
}

Size
bloompg_parallel_pack_memory(int parallel_workers)
{
	Size per_worker = mul_size(bloompg_pack_target_bytes(parallel_workers),
		(Size) 6);

	return mul_size(per_worker, (Size) Max(parallel_workers, 1));
}

static void
bloompg_pack_reserve_heap(BloomPGColumnVector *vector, Size required,
	Size limit_bytes)
{
	uint32 capacity = vector->heap_capacity;
	uint32 limit = (uint32) Min(Min((Size) UINT32_MAX, (Size) MaxAllocSize),
		limit_bytes);

	if (required <= capacity)
		return;
	if (required > limit || required > limit_bytes)
		ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("bloompg parallel vector packet exceeded its memory limit"),
			 errhint("Increase bloompg.materialization_memory, reduce bloompg.transfer_workers, or let PostgreSQL use the native plan.")));
	capacity = capacity == 0 ? 4096 : capacity;
	while ((Size) capacity < required)
	{
		if (capacity > limit / 2)
		{
			capacity = (uint32) required;
			break;
		}
		capacity *= 2;
	}
	if (vector->heap == NULL)
		vector->heap = palloc(capacity);
	else
		vector->heap = repalloc(vector->heap, capacity);
	vector->heap_capacity = capacity;
}

static Size
bloompg_pack_append(BloomPGColumnVector *vector, uint32 row, Datum value,
	bool is_null, Size max_growth, Size heap_limit)
{
	if (is_null)
		return 0;
	vector->validity[row / BLOOMPG_VALIDITY_BITS] |=
		UINT64CONST(1) << (row % BLOOMPG_VALIDITY_BITS);
	switch (vector->physical_type)
	{
		case BLOOMPG_VECTOR_BYVAL_1:
			((uint8 *) vector->data)[row] = (uint8) value;
			return 0;
		case BLOOMPG_VECTOR_BYVAL_2:
			((uint16 *) vector->data)[row] = (uint16) value;
			return 0;
		case BLOOMPG_VECTOR_BYVAL_4:
			((uint32 *) vector->data)[row] = (uint32) value;
			return 0;
		case BLOOMPG_VECTOR_BYVAL_8:
			((uint64 *) vector->data)[row] = (uint64) value;
			return 0;
		case BLOOMPG_VECTOR_BYREF:
		{
			const char *source;
			struct varlena *detoasted = NULL;
			Size size;
			Size offset = MAXALIGN(vector->heap_used);
			Size required;
			Size growth;

			if (vector->typlen == -1)
			{
				Size raw_size = toast_raw_datum_size(value);

				if (raw_size > max_growth)
					ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("bloompg parallel row exceeded its vector packet limit"),
						 errhint("Increase bloompg.materialization_memory, reduce bloompg.transfer_workers, or let PostgreSQL use the native plan.")));
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
			growth = required - vector->heap_used;
			if (growth > max_growth)
				ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bloompg parallel row exceeded its vector packet limit"),
					 errhint("Increase bloompg.materialization_memory, reduce bloompg.transfer_workers, or let PostgreSQL use the native plan.")));
			bloompg_pack_reserve_heap(vector, required, heap_limit);
			memcpy(vector->heap + offset, source, size);
			((uint32 *) vector->data)[row] = (uint32) offset;
			vector->heap_used = (uint32) required;
			if (detoasted != NULL &&
				(Pointer) detoasted != DatumGetPointer(value))
				pfree(detoasted);
			return growth;
		}
	}
	pg_unreachable();
}

static Node *
bloompg_create_pack_state(CustomScan *scan)
{
	BloomPGPackState *state = palloc0(sizeof(BloomPGPackState));

	(void) scan;
	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &bloompg_pack_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	return (Node *) state;
}

static void
bloompg_begin_pack(CustomScanState *custom_state, EState *estate, int eflags)
{
	BloomPGPackState *state = (BloomPGPackState *) custom_state;
	CustomScan *scan = castNode(CustomScan, custom_state->ss.ps.plan);
	int attribute;

	if (list_length(scan->custom_plans) != 1)
		elog(ERROR, "invalid bloompg vector-pack child plan");
	if (list_length(scan->custom_private) != 1 ||
		!IsA(linitial(scan->custom_private), Integer) ||
		intVal(linitial(scan->custom_private)) <= 0)
		elog(ERROR, "invalid bloompg vector-pack worker count");
	state->child = ExecInitNode((Plan *) linitial(scan->custom_plans), estate,
		eflags);
	state->css.custom_ps = list_make1(state->child);
	state->descriptor = ExecGetResultType(state->child);
	if (state->descriptor == NULL || state->descriptor->natts <= 0)
		elog(ERROR, "invalid bloompg vector-pack descriptor");
	state->vectors = palloc0_array(BloomPGColumnVector,
		state->descriptor->natts);
	state->packet_target = bloompg_pack_target_bytes(
		intVal(linitial(scan->custom_private)));
	state->packet_limit = mul_size(state->packet_target, (Size) 3);
	{
		Size vector_bytes = 0;

		for (attribute = 0; attribute < state->descriptor->natts; attribute++)
		{
			Form_pg_attribute form = TupleDescAttr(state->descriptor, attribute);
			BloomPGVectorPhysicalType physical_type =
				bloompg_pack_physical_type(form);

			vector_bytes = add_size(vector_bytes,
				mul_size(bloompg_pack_vector_width(physical_type),
					BLOOMPG_VECTOR_SIZE));
			vector_bytes = add_size(vector_bytes,
				sizeof(uint64) * BLOOMPG_VALIDITY_WORDS);
		}
		if (vector_bytes > state->packet_target)
			ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("bloompg parallel vector layout exceeded its memory limit"),
				 errhint("Increase bloompg.materialization_memory, reduce bloompg.transfer_workers, or let PostgreSQL use the native plan.")));
	}
	for (attribute = 0; attribute < state->descriptor->natts; attribute++)
	{
		BloomPGColumnVector *vector = &state->vectors[attribute];
		Form_pg_attribute form = TupleDescAttr(state->descriptor, attribute);
		Size width;

		vector->type_oid = form->atttypid;
		vector->typlen = form->attlen;
		vector->physical_type = bloompg_pack_physical_type(form);
		width = bloompg_pack_vector_width(vector->physical_type);
		vector->data = palloc(mul_size(width, BLOOMPG_VECTOR_SIZE));
		vector->validity = palloc0(sizeof(uint64) * BLOOMPG_VALIDITY_WORDS);
	}
}

static bytea *
bloompg_pack_serialize(BloomPGPackState *state, uint32 rows)
{
	Size header_size = add_size(sizeof(BloomPGPackedHeader),
		mul_size((Size) state->descriptor->natts,
			sizeof(BloomPGPackedVector)));
	Size size = MAXALIGN(header_size);
	Size validity_bytes = mul_size(
		(rows + BLOOMPG_VALIDITY_BITS - 1) / BLOOMPG_VALIDITY_BITS,
		sizeof(uint64));
	BloomPGPackedHeader header;
	char *base;
	int attribute;

	for (attribute = 0; attribute < state->descriptor->natts; attribute++)
	{
		BloomPGColumnVector *vector = &state->vectors[attribute];

		size = add_size(size, MAXALIGN(mul_size(rows,
			bloompg_pack_vector_width(vector->physical_type))));
		size = add_size(size, MAXALIGN(validity_bytes));
		if (vector->physical_type == BLOOMPG_VECTOR_BYREF)
			size = add_size(size, MAXALIGN(vector->heap_used));
	}
	if (size > UINT32_MAX || size > MaxAllocSize - VARHDRSZ ||
		size > state->packet_limit)
		ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("bloompg parallel vector packet exceeded its memory limit"),
			 errhint("Increase bloompg.materialization_memory, reduce bloompg.transfer_workers, or let PostgreSQL use the native plan.")));
	if (state->payload_capacity < add_size(VARHDRSZ, size))
	{
		state->payload_capacity = add_size(VARHDRSZ, size);
		state->payload = state->payload == NULL ?
			palloc(state->payload_capacity) :
			repalloc(state->payload, state->payload_capacity);
	}
	SET_VARSIZE(state->payload, VARHDRSZ + size);
	base = VARDATA(state->payload);
	header.magic = BLOOMPG_PACK_MAGIC;
	header.total_size = (uint32) size;
	header.rows = rows;
	header.natts = state->descriptor->natts;
	memcpy(base, &header, sizeof(header));
	size = MAXALIGN(header_size);
	for (attribute = 0; attribute < state->descriptor->natts; attribute++)
	{
		BloomPGColumnVector *vector = &state->vectors[attribute];
		BloomPGPackedVector metadata;
		Size data_bytes = mul_size(rows,
			bloompg_pack_vector_width(vector->physical_type));

		MemSet(&metadata, 0, sizeof(metadata));
		metadata.type_oid = vector->type_oid;
		metadata.typlen = vector->typlen;
		metadata.physical_type = (uint8) vector->physical_type;
		metadata.data_offset = (uint32) size;
		memcpy(base + size, vector->data, data_bytes);
		size += MAXALIGN(data_bytes);
		metadata.validity_offset = (uint32) size;
		memcpy(base + size, vector->validity, validity_bytes);
		size += MAXALIGN(validity_bytes);
		if (vector->physical_type == BLOOMPG_VECTOR_BYREF)
		{
			metadata.heap_offset = (uint32) size;
			metadata.heap_used = vector->heap_used;
			if (vector->heap_used > 0)
				memcpy(base + size, vector->heap, vector->heap_used);
			size += MAXALIGN(vector->heap_used);
		}
		memcpy(base + sizeof(BloomPGPackedHeader) +
			attribute * sizeof(BloomPGPackedVector), &metadata,
			sizeof(metadata));
	}
	Assert(size == header.total_size);
	return state->payload;
}

static TupleTableSlot *
bloompg_exec_pack(CustomScanState *custom_state)
{
	BloomPGPackState *state = (BloomPGPackState *) custom_state;
	TupleTableSlot *output = custom_state->ss.ss_ScanTupleSlot;
	uint32 rows = 0;
	Size heap_bytes = 0;
	int attribute;

	for (attribute = 0; attribute < state->descriptor->natts; attribute++)
	{
		BloomPGColumnVector *vector = &state->vectors[attribute];

		MemSet(vector->validity, 0,
			sizeof(uint64) * BLOOMPG_VALIDITY_WORDS);
		vector->heap_used = 0;
	}
	while (rows < BLOOMPG_VECTOR_SIZE)
	{
		TupleTableSlot *slot = ExecProcNode(state->child);
		Size row_heap_bytes = 0;

		if (TupIsNull(slot))
			break;
		slot_getallattrs(slot);
		for (attribute = 0; attribute < state->descriptor->natts; attribute++)
		{
			BloomPGColumnVector *vector = &state->vectors[attribute];
			uint32 previous_heap_used = vector->heap_used;

			row_heap_bytes = add_size(row_heap_bytes,
				bloompg_pack_append(&state->vectors[attribute], rows,
					slot->tts_values[attribute], slot->tts_isnull[attribute],
					state->packet_target - row_heap_bytes,
					state->packet_limit));
			heap_bytes = add_size(heap_bytes,
				(Size) vector->heap_used - previous_heap_used);
		}
		rows++;
		if (heap_bytes >= state->packet_target)
			break;
	}
	if (rows == 0)
		return NULL;
	ExecClearTuple(output);
	output->tts_values[0] = PointerGetDatum(bloompg_pack_serialize(state, rows));
	output->tts_isnull[0] = false;
	ExecStoreVirtualTuple(output);
	state->packets++;
	state->rows += rows;
	custom_state->ss.ps.ps_ExprContext->ecxt_scantuple = output;
	if (custom_state->ss.ps.ps_ProjInfo != NULL)
		return ExecProject(custom_state->ss.ps.ps_ProjInfo);
	return ExecCopySlot(custom_state->ss.ps.ps_ResultTupleSlot, output);
}

static void
bloompg_end_pack(CustomScanState *custom_state)
{
	BloomPGPackState *state = (BloomPGPackState *) custom_state;

	if (state->child != NULL)
		ExecEndNode(state->child);
}

static void
bloompg_rescan_pack(CustomScanState *custom_state)
{
	BloomPGPackState *state = (BloomPGPackState *) custom_state;

	if (state->child != NULL)
		ExecReScan(state->child);
}

static void
bloompg_explain_pack(CustomScanState *custom_state, List *ancestors,
	ExplainState *explain)
{
	BloomPGPackState *state = (BloomPGPackState *) custom_state;

	(void) ancestors;
	if (explain->analyze)
	{
		ExplainPropertyInteger("Vector Packets", NULL, state->packets, explain);
		ExplainPropertyInteger("Packed Rows", NULL, state->rows, explain);
	}
}

static bool
bloompg_pack_range_valid(Size total, uint32 offset, Size bytes)
{
	return (Size) offset <= total && bytes <= total - (Size) offset;
}

uint32
bloompg_receive_packed_chunk(Datum datum, BloomPGColumnStore *store,
	TupleDesc descriptor, BloomPGPackedChunkVisitor visitor,
	void *visitor_context)
{
	struct varlena *packed = (struct varlena *) PG_DETOAST_DATUM_PACKED(datum);
	const char *base = VARDATA_ANY(packed);
	Size total = VARSIZE_ANY_EXHDR(packed);
	BloomPGPackedHeader header;
	BloomPGColumnVector *vectors;
	Size validity_bytes;
	Size expected_offset;
	uint32 attribute;

	if (total < sizeof(BloomPGPackedHeader))
		elog(ERROR, "truncated bloompg vector packet");
	memcpy(&header, base, sizeof(header));
	if (header.magic != BLOOMPG_PACK_MAGIC || header.total_size != total ||
		header.rows == 0 || header.rows > BLOOMPG_VECTOR_SIZE ||
		header.natts != (uint32) descriptor->natts ||
		!bloompg_pack_range_valid(total, sizeof(BloomPGPackedHeader),
			mul_size(header.natts, sizeof(BloomPGPackedVector))))
		elog(ERROR, "invalid bloompg vector packet header");
	validity_bytes = mul_size(
		(header.rows + BLOOMPG_VALIDITY_BITS - 1) / BLOOMPG_VALIDITY_BITS,
		sizeof(uint64));
	expected_offset = MAXALIGN(add_size(sizeof(BloomPGPackedHeader),
		mul_size(header.natts, sizeof(BloomPGPackedVector))));
	if (expected_offset > total)
		elog(ERROR, "truncated bloompg vector packet metadata");
	vectors = palloc0_array(BloomPGColumnVector, header.natts);
	for (attribute = 0; attribute < header.natts; attribute++)
	{
		BloomPGPackedVector metadata;
		BloomPGColumnVector *vector = &vectors[attribute];
		Form_pg_attribute form = TupleDescAttr(descriptor, attribute);
		BloomPGVectorPhysicalType expected_type =
			bloompg_pack_physical_type(form);
		Size data_bytes;
		uint32 row;

		memcpy(&metadata, base + sizeof(BloomPGPackedHeader) +
			attribute * sizeof(BloomPGPackedVector), sizeof(metadata));
		if (metadata.type_oid != form->atttypid ||
			metadata.typlen != form->attlen ||
			metadata.physical_type != (uint8) expected_type ||
			metadata.reserved != 0)
			elog(ERROR, "invalid bloompg vector packet descriptor");
		vector->type_oid = metadata.type_oid;
		vector->typlen = metadata.typlen;
		vector->physical_type =
			(BloomPGVectorPhysicalType) metadata.physical_type;
		data_bytes = mul_size(header.rows,
			bloompg_pack_vector_width(vector->physical_type));
		if (metadata.data_offset != expected_offset ||
			!bloompg_pack_range_valid(total, metadata.data_offset, data_bytes))
			elog(ERROR, "invalid bloompg vector packet offsets");
		expected_offset = add_size(expected_offset, MAXALIGN(data_bytes));
		if (metadata.validity_offset != expected_offset ||
			!bloompg_pack_range_valid(total, metadata.validity_offset,
				validity_bytes))
			elog(ERROR, "invalid bloompg vector packet validity bitmap");
		expected_offset = add_size(expected_offset, MAXALIGN(validity_bytes));
		if (vector->physical_type == BLOOMPG_VECTOR_BYREF)
		{
			if (metadata.heap_offset != expected_offset ||
				!bloompg_pack_range_valid(total, metadata.heap_offset,
					metadata.heap_used))
				elog(ERROR, "invalid bloompg vector packet heap");
			expected_offset = add_size(expected_offset,
				MAXALIGN(metadata.heap_used));
		}
		else if (metadata.heap_offset != 0 || metadata.heap_used != 0)
			elog(ERROR, "invalid fixed-width bloompg vector heap");
		/* A bytea inside a MinimalTuple is only int-aligned.  Keep wide loads
		 * portable by copying the tiny validity bitmap, and copy by-reference
		 * offset arrays before the generic vector accessor casts them. */
		if (vector->physical_type == BLOOMPG_VECTOR_BYREF)
		{
			vector->data = palloc(data_bytes);
			memcpy(vector->data, base + metadata.data_offset, data_bytes);
		}
		else
			vector->data = (void *) (base + metadata.data_offset);
		vector->validity = palloc(validity_bytes);
		memcpy(vector->validity, base + metadata.validity_offset,
			validity_bytes);
		if (vector->physical_type == BLOOMPG_VECTOR_BYREF)
		{
			vector->heap = metadata.heap_used > 0 ?
				palloc(metadata.heap_used) : NULL;
			if (metadata.heap_used > 0)
				memcpy(vector->heap, base + metadata.heap_offset,
					metadata.heap_used);
			vector->heap_used = metadata.heap_used;
			vector->heap_capacity = metadata.heap_used;
			for (row = 0; row < header.rows; row++)
			{
				uint32 offset;
				Size datum_size;

				if (!bloompg_column_vector_is_valid(vector, row))
					continue;
				offset = ((const uint32 *) vector->data)[row];
				if ((offset % MAXIMUM_ALIGNOF) != 0 ||
					!bloompg_byref_datum_size(vector->heap,
						vector->heap_used, vector->typlen, offset,
						&datum_size))
					elog(ERROR, "invalid bloompg vector packet datum");
			}
		}
	}
	if (expected_offset != total)
		elog(ERROR, "invalid bloompg vector packet length");
	if (visitor != NULL)
		visitor(vectors, header.natts, header.rows, visitor_context);
	bloompg_column_store_append_dense(store, vectors, header.natts,
		header.rows);
	for (attribute = 0; attribute < header.natts; attribute++)
	{
		pfree(vectors[attribute].validity);
		if (vectors[attribute].physical_type == BLOOMPG_VECTOR_BYREF)
		{
			pfree(vectors[attribute].data);
			if (vectors[attribute].heap != NULL)
				pfree(vectors[attribute].heap);
		}
	}
	pfree(vectors);
	if ((Pointer) packed != DatumGetPointer(datum))
		pfree(packed);
	return header.rows;
}
