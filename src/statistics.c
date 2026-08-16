#include "postgres.h"

#include "bloompg/statistics.h"

#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "utils/memutils.h"

#include <math.h>

/* 4096 registers give an RSE of roughly 1.6% while using 4 KiB per key set. */
#define BLOOMPG_HLL_PRECISION 12
#define BLOOMPG_HLL_REGISTERS (1U << BLOOMPG_HLL_PRECISION)

typedef struct BloomPGStatisticsWork
{
	BloomPGJoinKeyStatistics *output;
	FmgrInfo   *hash_functions;
	uint8	   *registers;
} BloomPGStatisticsWork;

static double
bloompg_statistics_elapsed_ms(instr_time started)
{
	instr_time elapsed;

	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	return INSTR_TIME_GET_MILLISEC(elapsed);
}

static inline int
bloompg_hll_rank(uint64 value)
{
	int rank;

	if (value == 0)
		return 64 - BLOOMPG_HLL_PRECISION + 1;
#if defined(__GNUC__) || defined(__clang__)
	rank = __builtin_clzll(value) + 1 - BLOOMPG_HLL_PRECISION;
#else
	rank = 1;
	while ((value & (UINT64CONST(1) << 63)) == 0)
	{
		rank++;
		value <<= 1;
	}
	rank -= BLOOMPG_HLL_PRECISION;
#endif
	return Max(rank, 1);
}

static inline void
bloompg_hll_add(uint8 *registers, uint64 hash)
{
	uint32 index = (uint32) (hash & (BLOOMPG_HLL_REGISTERS - 1));
	uint64 remaining = hash >> BLOOMPG_HLL_PRECISION;
	int rank = bloompg_hll_rank(remaining);

	registers[index] = Max(registers[index], (uint8) rank);
}

static double
bloompg_hll_estimate(const uint8 *registers)
{
	const double m = (double) BLOOMPG_HLL_REGISTERS;
	double sum = 0.0;
	double estimate;
	uint32 zeroes = 0;
	uint32 index;

	for (index = 0; index < BLOOMPG_HLL_REGISTERS; index++)
	{
		sum += ldexp(1.0, -(int) registers[index]);
		if (registers[index] == 0)
			zeroes++;
	}
	estimate = 0.7213 / (1.0 + 1.079 / m) * m * m / sum;
	/* Linear counting is substantially less biased in the small range. */
	if (estimate <= 2.5 * m && zeroes > 0)
		estimate = m * log(m / (double) zeroes);
	return estimate;
}

static inline Size
bloompg_statistics_datum_width(int16 typlen, Datum value)
{
	if (typlen > 0)
		return (Size) typlen;
	if (typlen == -1)
		return (Size) VARSIZE_ANY(DatumGetPointer(value));
	if (typlen == -2)
		return strlen(DatumGetCString(value)) + 1;
	elog(ERROR, "invalid bloompg statistics datum width %d", typlen);
	pg_unreachable();
}

static inline void
bloompg_statistics_add_key(BloomPGStatisticsWork *work,
	const BloomPGColumnStoreChunkView *view, uint32 row,
	uint64 *values_examined)
{
	BloomPGJoinKeyStatistics *output = work->output;
	uint64 combined = 0;
	int key;

	for (key = 0; key < output->nkeys; key++)
	{
		AttrNumber attribute = output->materialized_attributes[key];
		Datum value;
		uint32 hash;

		(*values_examined)++;
		if (!bloompg_column_chunk_get_datum(view, attribute, row, &value))
		{
			output->null_rows++;
			return;
		}
		hash = DatumGetUInt32(FunctionCall1Coll(&work->hash_functions[key],
			output->collations[key], value));
		if (key == 0)
			combined = murmurhash64((uint64) hash);
		else
			combined = hash_combine64(combined, (uint64) hash);
	}
	output->nonnull_rows++;
	bloompg_hll_add(work->registers, murmurhash64(combined));
}

static inline void
bloompg_statistics_add_slot_key(BloomPGStatisticsWork *work,
	TupleTableSlot *slot, uint64 *values_examined)
{
	BloomPGJoinKeyStatistics *output = work->output;
	uint64 combined = 0;
	int key;

	for (key = 0; key < output->nkeys; key++)
	{
		bool is_null;
		Datum value = slot_getattr(slot,
			output->materialized_attributes[key], &is_null);
		uint32 hash;

		(*values_examined)++;
		if (is_null)
		{
			output->null_rows++;
			return;
		}
		hash = DatumGetUInt32(FunctionCall1Coll(&work->hash_functions[key],
			output->collations[key], value));
		if (key == 0)
			combined = murmurhash64((uint64) hash);
		else
			combined = hash_combine64(combined, (uint64) hash);
	}
	output->nonnull_rows++;
	bloompg_hll_add(work->registers, murmurhash64(combined));
}

static BloomPGStatisticsWork *
bloompg_statistics_begin_work(BloomPGMaterializedStatistics *statistics,
	int *work_count)
{
	BloomPGStatisticsWork *work;
	ListCell *cell;
	int index = 0;
	int maximum_work = 0;

	foreach(cell, statistics->join_keys)
	{
		BloomPGJoinKeyStatistics *output = lfirst(cell);

		if (!output->precomputed)
			maximum_work++;
	}
	*work_count = maximum_work;
	statistics->precomputed_key_sets =
		list_length(statistics->join_keys) - maximum_work;
	statistics->scanned_key_sets = maximum_work;
	if (maximum_work == 0)
		return NULL;
	work = palloc0_array(BloomPGStatisticsWork, maximum_work);
	foreach(cell, statistics->join_keys)
	{
		BloomPGJoinKeyStatistics *output = lfirst(cell);
		int key;

		if (output->precomputed)
			continue;
		work[index].output = output;
		work[index].hash_functions = palloc0_array(FmgrInfo, output->nkeys);
		work[index].registers = palloc0_array(uint8, BLOOMPG_HLL_REGISTERS);
		for (key = 0; key < output->nkeys; key++)
			fmgr_info(output->hash_procs[key],
				&work[index].hash_functions[key]);
		index++;
	}
	return work;
}

/* Slot-to-column conversion already accounts for every varlena byte.  That
 * total is a sufficiently accurate P1 row-width estimate and avoids another
 * pass over selected varlena values. */
static int32
bloompg_statistics_estimate_vector_width(BloomPGColumnStore *store,
	TupleDesc descriptor)
{
	double width = 0.0;
	uint64 stored_rows = bloompg_column_store_rows(store);
	int attribute;

	for (attribute = 0; attribute < descriptor->natts; attribute++)
	{
		Form_pg_attribute form = TupleDescAttr(descriptor, attribute);

		if (form->attlen > 0)
			width += form->attlen;
	}
	if (stored_rows > 0)
		width += (double) bloompg_column_store_varlen_bytes(store) /
			(double) stored_rows;
	return (int32) Min(rint(width), (double) INT_MAX);
}

static int32
bloompg_statistics_finish_width(TupleDesc descriptor, uint64 rows,
	const uint64 *variable_width_bytes, const uint64 *variable_nonnull)
{
	double width = 0.0;
	int attribute;

	for (attribute = 0; attribute < descriptor->natts; attribute++)
	{
		Form_pg_attribute form = TupleDescAttr(descriptor, attribute);

		if (form->attlen > 0)
			width += form->attlen;
		else if (variable_nonnull[attribute] > 0)
			width += (double) variable_width_bytes[attribute] /
				(double) variable_nonnull[attribute];
	}
	(void) rows;
	return (int32) Min(rint(width), (double) INT_MAX);
}

static void
bloompg_statistics_scan_vectors(BloomPGMaterializedStatistics *statistics,
	BloomPGColumnStore *store, BloomPGStatisticsWork *work, int work_count)
{
	uint32 chunk_count = bloompg_column_store_chunk_count(store);
	uint32 chunk_index;

	for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
	{
		BloomPGColumnStoreChunkView view;
		uint32 selected;

		if (!bloompg_column_store_chunk_view(store, chunk_index, &view))
			elog(ERROR, "bloompg statistics lost a vector chunk");
		for (selected = 0; selected < view.selected_count; selected++)
		{
			uint32 row = view.selection[selected];
			int index;

			for (index = 0; index < work_count; index++)
				bloompg_statistics_add_key(&work[index], &view, row,
					&statistics->values_examined);
			statistics->rows_examined++;
		}
		CHECK_FOR_INTERRUPTS();
	}
}

static void
bloompg_statistics_scan_spill(BloomPGMaterializedStatistics *statistics,
	BloomPGColumnStore *store, TupleDesc descriptor,
	BloomPGStatisticsWork *work, int work_count,
	uint64 *variable_width_bytes, uint64 *variable_nonnull)
{
	BloomPGColumnStoreScan scan;
	TupleTableSlot *slot = MakeSingleTupleTableSlot(descriptor, &TTSOpsVirtual);

	bloompg_column_store_begin_scan(store, &scan, 0);
	while (bloompg_column_store_next(store, &scan, true, slot, NULL))
	{
		int index;
		int attribute;

		for (index = 0; index < work_count; index++)
			bloompg_statistics_add_slot_key(&work[index], slot,
				&statistics->values_examined);
		slot_getallattrs(slot);
		for (attribute = 0; attribute < descriptor->natts; attribute++)
		{
			Form_pg_attribute form = TupleDescAttr(descriptor, attribute);

			if (form->attlen > 0 || slot->tts_isnull[attribute])
				continue;
			variable_width_bytes[attribute] += bloompg_statistics_datum_width(
				form->attlen, slot->tts_values[attribute]);
			variable_nonnull[attribute]++;
		}
		statistics->rows_examined++;
		ExecClearTuple(slot);
		if ((statistics->rows_examined & UINT64CONST(0xffff)) == 0)
			CHECK_FOR_INTERRUPTS();
	}
	bloompg_column_store_end_scan(&scan);
	ExecDropSingleTupleTableSlot(slot);
}

void
bloompg_statistics_finalize(BloomPGMaterializedStatistics *statistics,
	BloomPGColumnStore *store, TupleDesc descriptor)
{
	MemoryContext temporary;
	MemoryContext old_context;
	BloomPGStatisticsWork *work;
	uint64 *variable_width_bytes = NULL;
	uint64 *variable_nonnull = NULL;
	instr_time started;
	int work_count;
	int index;

	Assert(statistics != NULL);
	Assert(store != NULL);
	Assert(descriptor != NULL);
	INSTR_TIME_SET_CURRENT(started);
	statistics->rows = bloompg_column_store_live_rows(store);
	statistics->vectorized = !bloompg_column_store_spilled(store);
	temporary = AllocSetContextCreate(CurrentMemoryContext,
		"BloomPG statistics finalize", ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(temporary);
	work = bloompg_statistics_begin_work(statistics, &work_count);
	if (statistics->vectorized)
	{
		statistics->average_width =
			bloompg_statistics_estimate_vector_width(store, descriptor);
		statistics->width_estimated = true;
		if (work_count > 0)
			bloompg_statistics_scan_vectors(statistics, store, work,
				work_count);
	}
	else
	{
		variable_width_bytes = palloc0_array(uint64, descriptor->natts);
		variable_nonnull = palloc0_array(uint64, descriptor->natts);
		bloompg_statistics_scan_spill(statistics, store, descriptor, work,
			work_count, variable_width_bytes, variable_nonnull);
		statistics->average_width = bloompg_statistics_finish_width(descriptor,
			statistics->rows, variable_width_bytes, variable_nonnull);
	}
	for (index = 0; index < work_count; index++)
	{
		BloomPGJoinKeyStatistics *output = work[index].output;

		output->ndistinct = output->nonnull_rows == 0 ? 0.0 :
			Min(bloompg_hll_estimate(work[index].registers),
				(double) output->nonnull_rows);
	}
	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(temporary);
	statistics->finalize_ms = bloompg_statistics_elapsed_ms(started);
}
