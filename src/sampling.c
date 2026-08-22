#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/sampling.h"
#include "bloompg/scan_quals.h"

#include "catalog/pg_type_d.h"
#include "catalog/pg_class.h"
#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/table.h"
#include "access/tableam.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "executor/execdesc.h"
#include "executor/instrument.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "parser/parse_func.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/fd.h"
#include "tcop/dest.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct BloomPGPreparedSample
{
	Oid			relation_oid;
	Oid			relfilenode;
	uint64		qualifier_signature;
	int			sample_size;
	int			sample_seed;
	double		estimated_rows;
} BloomPGPreparedSample;

typedef struct BloomPGTidSample
{
	Oid			database_oid;
	Oid			relation_oid;
	Oid			relfilenode;
	int			sample_size;
	int			sample_seed;
	uint64		count;
	ItemPointerData *tids;
	/* The on-disk representation remains CTID-only.  A remembered backend
	 * sample may additionally hydrate snapshot-relaxed, fully detoasted tuple
	 * values once and reuse them for later planning invocations. */
	bool		memory_owned;
	bool		tuples_ready;
	MemoryContext tuple_context;
	TupleDesc	tuple_descriptor;
	MinimalTuple *tuples;
	uint64		tuple_count;
	Size		tid_memory_bytes;
	Size		tuple_memory_bytes;
} BloomPGTidSample;

/*
 * A prepared sample persisted on disk intentionally contains CTIDs rather
 * than tuple values.  In-memory tuple values are a relaxed planning cache:
 * they may become stale after DML, but can only change transfer scheduling
 * estimates.  Formal filters are always applied by an exact current-snapshot
 * scan, so query results never depend on this cache.  TOAST values are fully
 * flattened before a tuple outlives its originating snapshot.
 */
struct BloomPGSampleWorkspace
{
	MemoryContext memory_context;
	Oid			relation_oid;
	Oid			relfilenode;
	TupleDesc	descriptor;
	MinimalTuple *tuples;
	bool	   *local_accepted;
	bool	   *filter_accepted;
	List	   *applied_filter_tokens;
	bool		filter_state_ready;
	uint64		filter_rows;
	uint64		combined_rows;
	uint64		visible_rows;
	uint64		local_rows;
	double		load_ms;
	double		hydrate_ms;
	double		qual_ms;
	bool		tuple_cache_hit;
	uint64		probe_count;
	double		probe_ms;
	double		last_probe_ms;
	uint64		probe_rows_examined;
	uint64		probe_filter_checks;
	uint64		probe_filters_applied;
	uint64		probe_cache_hits;
};

typedef struct BloomPGTidReceiver
{
	DestReceiver receiver;
	ItemPointerData *tids;
	uint64		count;
	uint64		seen;
	uint64		capacity;
	pg_prng_state prng;
} BloomPGTidReceiver;

static List *prepared_samples = NIL;
static List *prepared_tid_samples = NIL;
static Size prepared_tid_cache_bytes = 0;
static Size prepared_tuple_cache_bytes = 0;
static Oid system_sample_handler = InvalidOid;
static Oid bernoulli_sample_handler = InvalidOid;

#define BLOOMPG_MAX_PREPARED_SAMPLE_ENTRIES 256
#define BLOOMPG_MAX_PREPARED_TID_SAMPLES 128

static Size
bloompg_sample_memory_cache_limit(void)
{
	Size materialization_bytes = mul_size(
		(Size) bloompg_materialization_memory_kb, (Size) 1024);

	return materialization_bytes / 2;
}

static double
bloompg_elapsed_ms(instr_time started)
{
	instr_time elapsed;

	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	return INSTR_TIME_GET_MILLISEC(elapsed);
}

static Oid
bloompg_system_sample_handler(void)
{
	if (!OidIsValid(system_sample_handler))
	{
		Oid argument_types[1] = {INTERNALOID};

		system_sample_handler = LookupFuncName(list_make2(makeString("pg_catalog"),
														 makeString("system")),
									 1, argument_types, false);
	}
	return system_sample_handler;
}

static Oid
bloompg_bernoulli_sample_handler(void)
{
	if (!OidIsValid(bernoulli_sample_handler))
	{
		Oid argument_types[1] = {INTERNALOID};

		bernoulli_sample_handler = LookupFuncName(
			list_make2(makeString("pg_catalog"), makeString("bernoulli")),
			1, argument_types, false);
	}
	return bernoulli_sample_handler;
}

static uint64
bloompg_qualifier_signature(Plan *scan)
{
	char *serialized = nodeToString(scan->qual);
	uint64 signature = hash_bytes_extended((unsigned char *) serialized,
												  strlen(serialized), 0);

	pfree(serialized);
	return signature;
}

static BloomPGPreparedSample *
bloompg_find_prepared_sample(Oid relation_oid, Oid relfilenode,
							uint64 signature)
{
	ListCell *cell;

	foreach(cell, prepared_samples)
	{
		BloomPGPreparedSample *sample = lfirst(cell);

		if (sample->relation_oid == relation_oid &&
			sample->relfilenode == relfilenode &&
			sample->qualifier_signature == signature &&
			sample->sample_size == bloompg_sample_size &&
			sample->sample_seed == bloompg_sample_seed)
			return sample;
	}
	return NULL;
}

static void
bloompg_store_prepared_sample(Oid relation_oid, Oid relfilenode,
							  uint64 signature,
							  double estimated_rows)
{
	BloomPGPreparedSample *sample;
	MemoryContext old_context;

	if (list_length(prepared_samples) >=
		BLOOMPG_MAX_PREPARED_SAMPLE_ENTRIES)
		return;
	old_context = MemoryContextSwitchTo(TopMemoryContext);

	sample = palloc0(sizeof(BloomPGPreparedSample));
	sample->relation_oid = relation_oid;
	sample->relfilenode = relfilenode;
	sample->qualifier_signature = signature;
	sample->sample_size = bloompg_sample_size;
	sample->sample_seed = bloompg_sample_seed;
	sample->estimated_rows = estimated_rows;
	prepared_samples = lappend(prepared_samples, sample);
	MemoryContextSwitchTo(old_context);
}

static Oid
bloompg_relation_filenode(Oid relation_oid)
{
	HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relation_oid));
	Oid relfilenode = InvalidOid;

	if (HeapTupleIsValid(tuple))
	{
		relfilenode = ((Form_pg_class) GETSTRUCT(tuple))->relfilenode;
		ReleaseSysCache(tuple);
	}
	return relfilenode;
}

static bool
bloompg_sample_cache_directory(char *path, Size path_size)
{
	if (bloompg_sample_cache_dir == NULL ||
		bloompg_sample_cache_dir[0] == '\0')
		return false;
	if (pg_strcasecmp(bloompg_sample_cache_dir, "auto") == 0)
		snprintf(path, path_size, "%s/bloompg_samples", DataDir);
	else if (is_absolute_path(bloompg_sample_cache_dir))
		strlcpy(path, bloompg_sample_cache_dir, path_size);
	else
		snprintf(path, path_size, "%s/%s", DataDir,
			bloompg_sample_cache_dir);
	return path[0] != '\0' && strlen(path) < path_size;
}

static bool
bloompg_sample_cache_path(char *path, Size path_size,
						 Oid relation_oid, Oid relfilenode,
						 uint64 qualifier_signature)
{
	char directory[MAXPGPATH];
	int written;

	if (!bloompg_sample_cache_directory(directory, sizeof(directory)))
		return false;
	written = snprintf(path, path_size,
		"%s/%u-%u-%d-%d-%016llx.sample", directory,
		relation_oid, relfilenode, bloompg_sample_size, bloompg_sample_seed,
		(unsigned long long) qualifier_signature);
	return written > 0 && (Size) written < path_size;
}

static bool
bloompg_tid_sample_cache_path(char *path, Size path_size,
							 Oid relation_oid, Oid relfilenode)
{
	char directory[MAXPGPATH];
	int written;

	if (!bloompg_sample_cache_directory(directory, sizeof(directory)))
		return false;
	written = snprintf(path, path_size,
		"%s/%u-%u-%u-%d-%d.tids", directory,
		MyDatabaseId, relation_oid, relfilenode,
		bloompg_sample_size, bloompg_sample_seed);
	return written > 0 && (Size) written < path_size;
}

static BloomPGTidSample *
bloompg_find_tid_sample(Oid relation_oid, Oid relfilenode)
{
	ListCell *cell;

	foreach(cell, prepared_tid_samples)
	{
		BloomPGTidSample *sample = lfirst(cell);

		if (sample->database_oid == MyDatabaseId &&
			sample->relation_oid == relation_oid &&
			sample->relfilenode == relfilenode &&
			sample->sample_size == bloompg_sample_size &&
			sample->sample_seed == bloompg_sample_seed)
			return sample;
	}
	return NULL;
}

static BloomPGTidSample *
bloompg_copy_tid_sample(BloomPGTidSample *source, MemoryContext context)
{
	BloomPGTidSample *copy;
	MemoryContext old_context = MemoryContextSwitchTo(context);

	copy = palloc0(sizeof(BloomPGTidSample));
	memcpy(copy, source, sizeof(BloomPGTidSample));
	copy->memory_owned = context == TopMemoryContext;
	copy->tuples_ready = false;
	copy->tuple_context = NULL;
	copy->tuple_descriptor = NULL;
	copy->tuples = NULL;
	copy->tuple_count = 0;
	copy->tid_memory_bytes = 0;
	copy->tuple_memory_bytes = 0;
	if (source->count > 0)
	{
		copy->tids = palloc_array(ItemPointerData, source->count);
		memcpy(copy->tids, source->tids,
			source->count * sizeof(ItemPointerData));
	}
	MemoryContextSwitchTo(old_context);
	return copy;
}

static void
bloompg_reset_tid_tuple_cache(BloomPGTidSample *sample)
{
	if (sample->tuple_memory_bytes > 0)
	{
		Assert(sample->tuple_memory_bytes <= prepared_tuple_cache_bytes);
		prepared_tuple_cache_bytes -= Min(sample->tuple_memory_bytes,
			prepared_tuple_cache_bytes);
	}
	if (sample->tuple_context != NULL)
		MemoryContextDelete(sample->tuple_context);
	sample->tuples_ready = false;
	sample->tuple_context = NULL;
	sample->tuple_descriptor = NULL;
	sample->tuples = NULL;
	sample->tuple_count = 0;
	sample->tuple_memory_bytes = 0;
}

static void
bloompg_free_tid_sample(BloomPGTidSample *sample)
{
	if (sample == NULL)
		return;
	bloompg_reset_tid_tuple_cache(sample);
	if (sample->tid_memory_bytes > 0)
	{
		Assert(sample->tid_memory_bytes <= prepared_tid_cache_bytes);
		prepared_tid_cache_bytes -= Min(sample->tid_memory_bytes,
			prepared_tid_cache_bytes);
	}
	if (sample->tids != NULL)
		pfree(sample->tids);
	pfree(sample);
}

static BloomPGTidSample *
bloompg_remember_tid_sample(BloomPGTidSample *source)
{
	BloomPGTidSample *copy;
	BloomPGTidSample *stale;
	MemoryContext old_context;
	Size cache_bytes;
	Size cache_limit;
	Size cache_used;

	if (!bloompg_sample_memory_cache)
		return source;
	copy = bloompg_find_tid_sample(source->relation_oid, source->relfilenode);
	if (copy != NULL)
		return copy;
	old_context = MemoryContextSwitchTo(TopMemoryContext);
	for (;;)
	{
		ListCell *cell;

		stale = NULL;
		foreach(cell, prepared_tid_samples)
		{
			BloomPGTidSample *candidate = lfirst(cell);

			if (candidate->database_oid == source->database_oid &&
				candidate->relation_oid == source->relation_oid)
			{
				stale = candidate;
				break;
			}
		}
		if (stale == NULL)
			break;
		prepared_tid_samples = list_delete_ptr(prepared_tid_samples, stale);
		bloompg_free_tid_sample(stale);
	}
	if (list_length(prepared_tid_samples) >=
		BLOOMPG_MAX_PREPARED_TID_SAMPLES)
	{
		MemoryContextSwitchTo(old_context);
		return source;
	}
	cache_bytes = add_size(sizeof(BloomPGTidSample),
		mul_size(source->count, sizeof(ItemPointerData)));
	cache_bytes = add_size(cache_bytes, sizeof(ListCell));
	cache_limit = bloompg_sample_memory_cache_limit();
	cache_used = add_size(prepared_tid_cache_bytes,
		prepared_tuple_cache_bytes);
	if (cache_bytes > cache_limit || cache_used > cache_limit - cache_bytes)
	{
		MemoryContextSwitchTo(old_context);
		return source;
	}
	copy = bloompg_copy_tid_sample(source, TopMemoryContext);
	prepared_tid_samples = lappend(prepared_tid_samples, copy);
	copy->tid_memory_bytes = cache_bytes;
	prepared_tid_cache_bytes = add_size(prepared_tid_cache_bytes,
		cache_bytes);
	MemoryContextSwitchTo(old_context);
	return copy;
}

static BloomPGTidSample *
bloompg_load_tid_sample(Oid relation_oid, Oid relfilenode)
{
	char path[MAXPGPATH];
	char magic[32];
	unsigned int file_relation;
	unsigned int file_node;
	unsigned int file_database;
	unsigned long long file_count;
	int file_size;
	int file_seed;
	FILE *file;
	BloomPGTidSample *sample;
	uint64 i;

	if (!bloompg_tid_sample_cache_path(path, sizeof(path), relation_oid,
		relfilenode))
		return NULL;
	file = AllocateFile(path, "r");
	if (file == NULL)
		return NULL;
	if (fscanf(file, "%31s %u %u %u %d %d %llu",
			magic, &file_database, &file_relation, &file_node,
			&file_size, &file_seed, &file_count) != 7 ||
		strcmp(magic, "BLOOMPG_TID_SAMPLE_V3") != 0 ||
		file_database != MyDatabaseId || file_relation != relation_oid ||
		file_node != relfilenode ||
		file_size != bloompg_sample_size || file_seed != bloompg_sample_seed ||
		file_count > (unsigned long long) bloompg_sample_size)
	{
		FreeFile(file);
		return NULL;
	}
	sample = palloc0(sizeof(BloomPGTidSample));
	sample->database_oid = MyDatabaseId;
	sample->relation_oid = relation_oid;
	sample->relfilenode = relfilenode;
	sample->sample_size = file_size;
	sample->sample_seed = file_seed;
	sample->count = (uint64) file_count;
	if (sample->count > 0)
		sample->tids = palloc_array(ItemPointerData, sample->count);
	for (i = 0; i < sample->count; i++)
	{
		unsigned int block;
		unsigned int offset;

		if (fscanf(file, "%u %u", &block, &offset) != 2 ||
			!BlockNumberIsValid((BlockNumber) block) ||
			offset == 0 || offset > MaxOffsetNumber)
		{
			FreeFile(file);
			if (sample->tids != NULL)
				pfree(sample->tids);
			pfree(sample);
			return NULL;
		}
		ItemPointerSet(&sample->tids[i], (BlockNumber) block,
			(OffsetNumber) offset);
	}
	FreeFile(file);
	return sample;
}

static void
bloompg_store_tid_sample(BloomPGTidSample *sample)
{
	char directory[MAXPGPATH];
	char path[MAXPGPATH];
	char temporary[MAXPGPATH];
	FILE *file;
	uint64 i;
	bool failed = false;

	if (!bloompg_sample_cache_directory(directory, sizeof(directory)) ||
		!bloompg_tid_sample_cache_path(path, sizeof(path),
			sample->relation_oid, sample->relfilenode))
		return;
	if (MakePGDirectory(directory) < 0 && errno != EEXIST)
		return;
	if (snprintf(temporary, sizeof(temporary), "%s.%d.tmp", path,
			MyProcPid) >= (int) sizeof(temporary))
		return;
	file = AllocateFile(temporary, "w");
	if (file == NULL)
		return;
	if (fprintf(file, "BLOOMPG_TID_SAMPLE_V3 %u %u %u %d %d " UINT64_FORMAT "\n",
			sample->database_oid, sample->relation_oid, sample->relfilenode,
			sample->sample_size, sample->sample_seed,
			sample->count) < 0)
		failed = true;
	for (i = 0; !failed && i < sample->count; i++)
	{
		if (fprintf(file, "%u %u\n",
				ItemPointerGetBlockNumber(&sample->tids[i]),
				ItemPointerGetOffsetNumber(&sample->tids[i])) < 0)
			failed = true;
	}
	if (FreeFile(file) != 0)
		failed = true;
	if (failed || rename(temporary, path) != 0)
		(void) unlink(temporary);
}

static bool
bloompg_load_disk_sample(Oid relation_oid, Oid relfilenode,
						 uint64 qualifier_signature, double *estimated_rows)
{
	char path[MAXPGPATH];
	char magic[32];
	unsigned int file_relation;
	unsigned int file_node;
	unsigned long long file_signature;
	int file_size;
	int file_seed;
	double file_estimate;
	FILE *file;
	int fields;

	if (!bloompg_sample_cache_path(path, sizeof(path), relation_oid,
		relfilenode, qualifier_signature))
		return false;
	file = AllocateFile(path, "r");
	if (file == NULL)
		return false;
	fields = fscanf(file, "%31s %u %u %d %d %llx %lf",
		magic, &file_relation, &file_node, &file_size, &file_seed,
		&file_signature, &file_estimate);
	FreeFile(file);
	if (fields != 7 || strcmp(magic, "BLOOMPG_SAMPLE_V1") != 0 ||
		file_relation != relation_oid || file_node != relfilenode ||
		file_size != bloompg_sample_size || file_seed != bloompg_sample_seed ||
		(uint64) file_signature != qualifier_signature ||
		!isfinite(file_estimate) || file_estimate < 0.0)
		return false;
	*estimated_rows = file_estimate;
	return true;
}

static void
bloompg_store_disk_sample(Oid relation_oid, Oid relfilenode,
						  uint64 qualifier_signature, double estimated_rows)
{
	char directory[MAXPGPATH];
	char path[MAXPGPATH];
	char temporary[MAXPGPATH];
	char contents[256];
	int descriptor;
	int length;
	ssize_t written;

	if (!bloompg_sample_cache_directory(directory, sizeof(directory)) ||
		!bloompg_sample_cache_path(path, sizeof(path), relation_oid,
			relfilenode, qualifier_signature))
		return;
	if (MakePGDirectory(directory) < 0 && errno != EEXIST)
		return;
	if (snprintf(temporary, sizeof(temporary), "%s.%d.tmp", path,
			 MyProcPid) >= (int) sizeof(temporary))
		return;
	descriptor = OpenTransientFilePerm(temporary,
		O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY, S_IRUSR | S_IWUSR);
	if (descriptor < 0)
		return;
	length = snprintf(contents, sizeof(contents),
		"BLOOMPG_SAMPLE_V1 %u %u %d %d %016llx %.17g\n",
		relation_oid, relfilenode, bloompg_sample_size, bloompg_sample_seed,
		(unsigned long long) qualifier_signature, estimated_rows);
	written = write(descriptor, contents, length);
	if (CloseTransientFile(descriptor) != 0 || written != length ||
		rename(temporary, path) != 0)
		(void) unlink(temporary);
}

static bool
bloompg_receive_tid_slot(TupleTableSlot *slot, DestReceiver *receiver)
{
	BloomPGTidReceiver *tid_receiver = (BloomPGTidReceiver *) receiver;
	bool is_null;
	Datum datum = slot_getattr(slot, 1, &is_null);
	uint64 selected;

	if (is_null)
		return true;
	tid_receiver->seen++;
	if (tid_receiver->count < tid_receiver->capacity)
		selected = tid_receiver->count++;
	else
	{
		selected = pg_prng_uint64_range(&tid_receiver->prng, 0,
			tid_receiver->seen - 1);
		if (selected >= tid_receiver->capacity)
			return true;
	}
	ItemPointerCopy(DatumGetItemPointer(datum), &tid_receiver->tids[selected]);
	return true;
}

static void
bloompg_tid_receiver_startup(DestReceiver *receiver, int operation,
							 TupleDesc descriptor)
{
	(void) receiver;
	(void) operation;
	(void) descriptor;
}

static void
bloompg_tid_receiver_shutdown(DestReceiver *receiver)
{
	(void) receiver;
}

static void
bloompg_tid_receiver_destroy(DestReceiver *receiver)
{
	pfree(receiver);
}

static BloomPGTidReceiver *
bloompg_create_tid_receiver(uint64 capacity, uint64 seed)
{
	BloomPGTidReceiver *receiver = palloc0(sizeof(BloomPGTidReceiver));

	receiver->receiver.receiveSlot = bloompg_receive_tid_slot;
	receiver->receiver.rStartup = bloompg_tid_receiver_startup;
	receiver->receiver.rShutdown = bloompg_tid_receiver_shutdown;
	receiver->receiver.rDestroy = bloompg_tid_receiver_destroy;
	receiver->receiver.mydest = DestNone;
	receiver->capacity = capacity;
	receiver->tids = palloc_array(ItemPointerData, capacity);
	pg_prng_seed(&receiver->prng, seed);
	return receiver;
}

static SampleScan *
bloompg_make_tid_sample_scan(Plan *source, Index rti, double base_rows,
							 bool repeatable, uint64 repeatable_seed)
{
	SampleScan *sample = makeNode(SampleScan);
	Plan *plan = &sample->scan.plan;
	TableSampleClause *clause = makeNode(TableSampleClause);
	double percentage = repeatable ? 100.0 : (base_rows > 0 ?
		150.0 * (double) bloompg_sample_size / base_rows : 100.0);
	float4 percentage_float;

	percentage = Max(0.000001, Min(percentage, 100.0));
	percentage_float = (float4) percentage;
	plan->startup_cost = source->startup_cost;
	plan->total_cost = source->total_cost;
	plan->plan_rows = Min(base_rows, (double) bloompg_sample_size);
	plan->plan_width = sizeof(ItemPointerData);
	plan->parallel_aware = false;
	plan->parallel_safe = false;
	plan->async_capable = false;
	plan->plan_node_id = source->plan_node_id;
	plan->targetlist = list_make1(makeTargetEntry(
		(Expr *) makeVar(rti, SelfItemPointerAttributeNumber, TIDOID,
			-1, InvalidOid, 0), 1, "ctid", false));
	plan->qual = NIL;
	plan->initPlan = NIL;
	plan->extParam = NULL;
	plan->allParam = NULL;
	sample->scan.scanrelid = rti;
	/* Prepared samples are persisted and reused, so pay the one-time full
	 * heap pass needed by BERNOULLI to avoid the severe block-correlation
	 * bias of SYSTEM on clustered range columns. */
	clause->tsmhandler = repeatable ? bloompg_bernoulli_sample_handler() :
		bloompg_system_sample_handler();
	clause->args = list_make1(makeConst(FLOAT4OID, -1, InvalidOid,
		sizeof(float4), Float4GetDatum(percentage_float), false, true));
	if (repeatable)
		clause->repeatable = (Expr *) makeConst(FLOAT8OID, -1, InvalidOid,
			sizeof(float8), Float8GetDatum((float8) repeatable_seed),
			false, FLOAT8PASSBYVAL);
	sample->tablesample = clause;
	return sample;
}

static BloomPGTidSample *
bloompg_build_tid_sample(PlannedStmt *candidate, Index rti, Plan *scan,
						 double base_rows, Oid relation_oid, Oid relfilenode,
						 const char *query_string, ParamListInfo bound_params,
						 bool repeatable, uint64 repeatable_seed)
{
	PlannedStmt *detached = copyObject(candidate);
	BloomPGTidReceiver *receiver;
	QueryDesc *query_desc = NULL;
	BloomPGTidSample *sample;
	uint64 receiver_seed = hash_combine64(repeatable_seed,
		UINT64CONST(0x7f4a7c159e3779b9));

	detached->planTree = (Plan *) bloompg_make_tid_sample_scan(scan, rti,
		base_rows, repeatable, repeatable_seed);
	detached->canSetTag = false;
	detached->parallelModeNeeded = false;
	detached->rowMarks = NIL;
	detached->resultRelations = NIL;
	detached->jitFlags = 0;
	receiver = bloompg_create_tid_receiver((uint64) bloompg_sample_size,
		receiver_seed);
	PG_TRY();
	{
		query_desc = CreateQueryDesc(detached,
			query_string != NULL ? query_string : "<bloompg prepared sample>",
			GetActiveSnapshot(), InvalidSnapshot, (DestReceiver *) receiver,
			bound_params, NULL, 0);
		ExecutorStart(query_desc, 0);
		ExecutorRun(query_desc, ForwardScanDirection, 0);
		ExecutorFinish(query_desc);
		ExecutorEnd(query_desc);
		FreeQueryDesc(query_desc);
		query_desc = NULL;
	}
	PG_CATCH();
	{
		/* The optimizer subtransaction abort owns failed executor cleanup. */
		PG_RE_THROW();
	}
	PG_END_TRY();
	sample = palloc0(sizeof(BloomPGTidSample));
	sample->database_oid = MyDatabaseId;
	sample->relation_oid = relation_oid;
	sample->relfilenode = relfilenode;
	sample->sample_size = bloompg_sample_size;
	sample->sample_seed = bloompg_sample_seed;
	sample->count = receiver->count;
	sample->tids = receiver->tids;
	receiver->tids = NULL;
	receiver->receiver.rDestroy((DestReceiver *) receiver);
	return sample;
}

/*
 * An InitPlan leaves no SubPlan node behind in the qualifier itself: the
 * SubPlan hangs off the Plan and the qualifier only references its output
 * through a PARAM_EXEC Param.  contain_subplans() cannot see that, and the
 * synthetic state below never fills es_param_exec_vals, so evaluating such a
 * Param would dereference an absent parameter slot.
 */
static bool
bloompg_exec_param_walker(Node *node, void *context)
{
	(void) context;
	if (node == NULL)
		return false;
	if (IsA(node, Param))
		return castNode(Param, node)->paramkind == PARAM_EXEC;
	return expression_tree_walker(node, bloompg_exec_param_walker, NULL);
}

/*
 * A query-local sample workspace deliberately uses a lightweight synthetic
 * PlanState instead of starting a complete QueryDesc.  Such a state has no
 * es_subplanstates, so asking ExecInitQual() to initialize a SubPlan would
 * dereference an absent executor subplan list.  Subquery predicates are safe
 * to omit here: sampling only controls estimates and cascade scheduling, while
 * the formal materialization scan executes the original plan (including every
 * SubPlan and InitPlan) under the current snapshot.  Keep independent top-level
 * conjuncts so column/local selectivity remains useful whenever possible.
 */
static List *
bloompg_sampling_safe_quals(List *quals)
{
	List *result = NIL;
	ListCell *cell;

	foreach(cell, quals)
	{
		Node *qual = lfirst(cell);

		if (contain_subplans(qual) ||
			bloompg_exec_param_walker(qual, NULL))
			continue;
		result = lappend(result, qual);
	}
	return result;
}

static Size
bloompg_sample_slot_flattened_size(TupleTableSlot *slot)
{
	TupleDesc descriptor = slot->tts_tupleDescriptor;
	Size bytes = MAXALIGN(sizeof(MinimalTupleData));
	int attribute;

	slot_getallattrs(slot);
	for (attribute = 0; attribute < descriptor->natts; attribute++)
	{
		Form_pg_attribute form = TupleDescAttr(descriptor, attribute);

		if (form->attisdropped || slot->tts_isnull[attribute])
			continue;
		if (form->attbyval)
			bytes = add_size(bytes, (Size) form->attlen);
		else if (form->attlen == -1)
			bytes = add_size(bytes,
				toast_raw_datum_size(slot->tts_values[attribute]));
		else if (form->attlen == -2)
			bytes = add_size(bytes,
				strlen(DatumGetCString(slot->tts_values[attribute])) + 1);
		else if (form->attlen > 0)
			bytes = add_size(bytes, (Size) form->attlen);
		else
			elog(ERROR, "invalid sample attribute width %d", form->attlen);
	}
	return bytes;
}

static BloomPGSampleWorkspace *
bloompg_build_sample_workspace(BloomPGTidSample *sample, Plan *scan,
								   Oid relfilenode,
								   ParamListInfo bound_params)
{
	MemoryContext parent_context = CurrentMemoryContext;
	MemoryContext workspace_context;
	MemoryContext old_context;
	BloomPGSampleWorkspace *workspace;
	List *quals;
	Relation relation;
	EState *estate;
	PlanState plan_state;
	ExprState *qual_state;
	ExprContext *expression_context;
	TupleTableSlot *slot;
	instr_time started;
	instr_time elapsed;
	uint64 i;
	bool use_tuple_cache;
	Size cache_limit;
	Size cache_used;

	workspace_context = AllocSetContextCreate(parent_context,
		"BloomPG query-local sample", ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(workspace_context);
	workspace = palloc0(sizeof(BloomPGSampleWorkspace));
	workspace->memory_context = workspace_context;
	workspace->relation_oid = sample->relation_oid;
	workspace->relfilenode = relfilenode;
	quals = bloompg_sampling_safe_quals(bloompg_scan_base_quals(scan));
	INSTR_TIME_SET_CURRENT(started);

	relation = table_open(sample->relation_oid, AccessShareLock);
	if (sample->memory_owned && sample->tuples_ready &&
		!equalTupleDescs(sample->tuple_descriptor, RelationGetDescr(relation)))
		bloompg_reset_tid_tuple_cache(sample);
	cache_limit = bloompg_sample_memory_cache_limit();
	cache_used = add_size(prepared_tid_cache_bytes,
		prepared_tuple_cache_bytes);
	use_tuple_cache = sample->memory_owned &&
		(sample->tuples_ready || cache_used < cache_limit);
	if (use_tuple_cache && !sample->tuples_ready)
	{
		MemoryContext cache_context;
		MemoryContext prior_context;
		TupleTableSlot *fetch_slot;
		instr_time hydrate_started;
		Size cache_remaining = cache_limit - cache_used;
		bool cache_abandoned = false;

		INSTR_TIME_SET_CURRENT(hydrate_started);
		cache_context = AllocSetContextCreate(TopMemoryContext,
			"BloomPG hydrated TID sample", ALLOCSET_DEFAULT_SIZES);
		prior_context = MemoryContextSwitchTo(cache_context);
		sample->tuple_context = cache_context;
		/* Keep constraint metadata too: equalTupleDescs() deliberately checks
		 * NOT NULL/default/generated state, while CreateTupleDescCopy() clears
		 * those fields and would make every later statement look like DDL. */
		sample->tuple_descriptor =
			CreateTupleDescCopyConstr(RelationGetDescr(relation));
		if (sample->count > 0)
			sample->tuples = palloc0_array(MinimalTuple, sample->count);
		MemoryContextSwitchTo(prior_context);

		fetch_slot = table_slot_create(relation, NULL);
		PG_TRY();
		{
			for (i = 0; i < sample->count; i++)
			{
				HeapTuple heap_tuple;
				HeapTuple flattened_tuple;
				Size tuple_bytes;

				ExecClearTuple(fetch_slot);
				if (!table_tuple_fetch_row_version(relation, &sample->tids[i],
						GetActiveSnapshot(), fetch_slot))
					continue;
				tuple_bytes = bloompg_sample_slot_flattened_size(fetch_slot);
				if (tuple_bytes > cache_remaining ||
					MemoryContextMemAllocated(cache_context, false) >
						cache_remaining - tuple_bytes)
				{
					cache_abandoned = true;
					break;
				}
				heap_tuple = ExecCopySlotHeapTuple(fetch_slot);
				if (HeapTupleHasExternal(heap_tuple))
				{
					flattened_tuple = toast_flatten_tuple(heap_tuple,
						sample->tuple_descriptor);
					heap_freetuple(heap_tuple);
				}
				else
					flattened_tuple = heap_tuple;
				prior_context = MemoryContextSwitchTo(cache_context);
				sample->tuples[sample->tuple_count++] =
					minimal_tuple_from_heap_tuple(flattened_tuple, 0);
				MemoryContextSwitchTo(prior_context);
				heap_freetuple(flattened_tuple);
				if (MemoryContextMemAllocated(cache_context, false) >
					cache_remaining)
				{
					cache_abandoned = true;
					break;
				}
			}
			ExecClearTuple(fetch_slot);
			ExecDropSingleTupleTableSlot(fetch_slot);
			fetch_slot = NULL;
			if (!cache_abandoned)
			{
				Size tuple_memory_bytes =
					MemoryContextMemAllocated(cache_context, false);
				Size new_cache_bytes = add_size(prepared_tuple_cache_bytes,
					tuple_memory_bytes);

				sample->tuple_memory_bytes = tuple_memory_bytes;
				prepared_tuple_cache_bytes = new_cache_bytes;
				sample->tuples_ready = true;
			}
			else
			{
				bloompg_reset_tid_tuple_cache(sample);
				use_tuple_cache = false;
			}
		}
		PG_CATCH();
		{
			if (fetch_slot != NULL)
			{
				ExecClearTuple(fetch_slot);
				ExecDropSingleTupleTableSlot(fetch_slot);
			}
			bloompg_reset_tid_tuple_cache(sample);
			PG_RE_THROW();
		}
		PG_END_TRY();
		workspace->hydrate_ms = bloompg_elapsed_ms(hydrate_started);
	}
	else if (use_tuple_cache)
		workspace->tuple_cache_hit = true;

	workspace->descriptor = CreateTupleDescCopy(use_tuple_cache ?
		sample->tuple_descriptor : RelationGetDescr(relation));
	if (sample->count > 0)
	{
		workspace->tuples = palloc0_array(MinimalTuple, sample->count);
		workspace->local_accepted = palloc0_array(bool, sample->count);
	}
	estate = CreateExecutorState();
	estate->es_snapshot = GetActiveSnapshot();
	estate->es_param_list_info = bound_params;
	MemSet(&plan_state, 0, sizeof(plan_state));
	plan_state.state = estate;
	ExecAssignExprContext(estate, &plan_state);
	expression_context = plan_state.ps_ExprContext;
	qual_state = quals != NIL ? ExecInitQual(quals, &plan_state) : NULL;
	slot = use_tuple_cache ?
		MakeSingleTupleTableSlot(workspace->descriptor, &TTSOpsMinimalTuple) :
		table_slot_create(relation, NULL);
	INSTR_TIME_SET_CURRENT(elapsed);
	for (i = 0; i < (use_tuple_cache ? sample->tuple_count : sample->count); i++)
	{
		MinimalTuple tuple;
		uint64 visible_index;
		bool local_passed;

		ResetExprContext(expression_context);
		ExecClearTuple(slot);
		if (use_tuple_cache)
			ExecStoreMinimalTuple(sample->tuples[i], slot, false);
		else if (!table_tuple_fetch_row_version(relation, &sample->tids[i],
					 GetActiveSnapshot(), slot))
			continue;
		visible_index = workspace->visible_rows++;
		tuple = use_tuple_cache ? sample->tuples[i] :
			ExecCopySlotMinimalTuple(slot);
		workspace->tuples[visible_index] = tuple;
		expression_context->ecxt_scantuple = slot;
		local_passed = qual_state == NULL || ExecQual(qual_state,
			expression_context);
		workspace->local_accepted[visible_index] = local_passed;
		if (local_passed)
			workspace->local_rows++;
	}
	workspace->qual_ms = bloompg_elapsed_ms(elapsed);
	ExecClearTuple(slot);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	table_close(relation, AccessShareLock);
	workspace->load_ms = bloompg_elapsed_ms(started);
	MemoryContextSwitchTo(old_context);
	return workspace;
}

static bool
bloompg_workspace_filter_prefix_matches(BloomPGSampleWorkspace *workspace,
	List *filters)
{
	ListCell *token_cell;
	ListCell *filter_cell = list_head(filters);

	foreach(token_cell, workspace->applied_filter_tokens)
	{
		uint64 *token = lfirst(token_cell);
		BloomPGSampleFilter *filter;

		if (filter_cell == NULL)
			return false;
		filter = lfirst(filter_cell);
		if (*token != filter->token)
			return false;
		filter_cell = lnext(filters, filter_cell);
	}
	return true;
}

static void
bloompg_workspace_reset_filter_state(BloomPGSampleWorkspace *workspace)
{
	MemoryContext old_context = MemoryContextSwitchTo(workspace->memory_context);

	if (workspace->visible_rows > 0 && workspace->filter_accepted == NULL)
		workspace->filter_accepted = palloc_array(bool, workspace->visible_rows);
	if (workspace->filter_accepted != NULL)
		MemSet(workspace->filter_accepted, true,
			sizeof(bool) * workspace->visible_rows);
	list_free_deep(workspace->applied_filter_tokens);
	workspace->applied_filter_tokens = NIL;
	workspace->filter_rows = workspace->visible_rows;
	workspace->combined_rows = workspace->local_rows;
	workspace->filter_state_ready = true;
	MemoryContextSwitchTo(old_context);
}

/*
 * Incoming formal filters are append-only until this relation is materialized.
 * Preserve a survivor mask in the query-local sample and evaluate only the new
 * suffix.  Estimates are identical to replaying the complete list: filter_rows
 * is the independent filter population and combined_rows also requires the
 * original local qual.  A defensive prefix check rebuilds the state if a
 * future caller ever supplies a non-append list.
 */
static bool
bloompg_estimate_from_workspace(BloomPGSampleWorkspace *workspace,
								double base_rows, List *filters,
								double *estimated_rows,
								double *filter_fraction)
{
	MemoryContext old_context;
	TupleTableSlot *slot = NULL;
	uint64 passed;
	uint64 filter_passed;
	uint64 i;
	instr_time started;
	instr_time elapsed;
	ListCell *new_filter_cell;
	ListCell *cell;
	int applied_count;
	int filter_index;

	if (workspace->visible_rows == 0)
		return false;
	INSTR_TIME_SET_CURRENT(started);
	if (!workspace->filter_state_ready ||
		!bloompg_workspace_filter_prefix_matches(workspace, filters))
		bloompg_workspace_reset_filter_state(workspace);
	if (filters == NIL)
	{
		if (workspace->applied_filter_tokens != NIL)
			bloompg_workspace_reset_filter_state(workspace);
	}
	else
	{
		applied_count = list_length(workspace->applied_filter_tokens);
		new_filter_cell = list_head(filters);
		for (filter_index = 0; filter_index < applied_count; filter_index++)
			new_filter_cell = lnext(filters, new_filter_cell);
		if (new_filter_cell == NULL)
			workspace->probe_cache_hits++;
		else
		{
			old_context = MemoryContextSwitchTo(workspace->memory_context);
			slot = MakeSingleTupleTableSlot(workspace->descriptor,
				&TTSOpsMinimalTuple);
			for (i = 0; i < workspace->visible_rows; i++)
			{
				bool accepted = true;

				if (!workspace->filter_accepted[i])
					continue;
				workspace->probe_rows_examined++;
				ExecStoreMinimalTuple(workspace->tuples[i], slot, false);
				for (cell = new_filter_cell; cell != NULL;
					 cell = lnext(filters, cell))
				{
					BloomPGSampleFilter *sample_filter = lfirst(cell);

					workspace->probe_filter_checks++;
					if (!bloompg_filter_contains_slot(sample_filter->filter, slot,
							sample_filter->attributes))
					{
						accepted = false;
						break;
					}
				}
				if (!accepted)
				{
					workspace->filter_accepted[i] = false;
					workspace->filter_rows--;
					if (workspace->local_accepted[i])
						workspace->combined_rows--;
				}
				ExecClearTuple(slot);
			}
			ExecDropSingleTupleTableSlot(slot);
			for (cell = new_filter_cell; cell != NULL;
				 cell = lnext(filters, cell))
			{
				BloomPGSampleFilter *sample_filter = lfirst(cell);
				uint64 *token = palloc(sizeof(uint64));

				*token = sample_filter->token;
				workspace->applied_filter_tokens = lappend(
					workspace->applied_filter_tokens, token);
				workspace->probe_filters_applied++;
			}
			MemoryContextSwitchTo(old_context);
		}
	}
	passed = workspace->combined_rows;
	filter_passed = workspace->filter_rows;
	INSTR_TIME_SET_CURRENT(elapsed);
	INSTR_TIME_SUBTRACT(elapsed, started);
	workspace->last_probe_ms = INSTR_TIME_GET_MILLISEC(elapsed);
	workspace->probe_count++;
	workspace->probe_ms += workspace->last_probe_ms;
	*estimated_rows = base_rows * (double) passed /
		(double) workspace->visible_rows;
	*estimated_rows = Max(*estimated_rows, passed > 0 ? 1.0 : 0.0);
	if (filter_fraction != NULL)
		*filter_fraction = (double) filter_passed /
			(double) workspace->visible_rows;
	return true;
}

bool
bloompg_sample_workspace_order_costs(BloomPGSampleWorkspace *workspace,
	double *local_sample_ms, double *filter_sample_ms)
{
	if (workspace == NULL || workspace->visible_rows == 0 ||
		workspace->probe_count == 0)
		return false;
	if (local_sample_ms != NULL)
		*local_sample_ms = workspace->qual_ms;
	if (filter_sample_ms != NULL)
		*filter_sample_ms = workspace->last_probe_ms;
	return true;
}

bool
bloompg_sample_workspace_filter_fraction(BloomPGSampleWorkspace *workspace,
	BloomPGSampleFilter *filter, double *fraction, uint64 *rows_examined,
	uint64 *rows_matched, double *probe_ms)
{
	TupleTableSlot *slot;
	uint64 matched = 0;
	uint64 index;
	instr_time started;

	if (fraction != NULL)
		*fraction = 1.0;
	if (rows_examined != NULL)
		*rows_examined = 0;
	if (rows_matched != NULL)
		*rows_matched = 0;
	if (probe_ms != NULL)
		*probe_ms = 0.0;
	if (workspace == NULL || workspace->visible_rows == 0 || filter == NULL ||
		filter->filter == NULL || !filter->filter->valid ||
		filter->attributes == NULL)
		return false;

	INSTR_TIME_SET_CURRENT(started);
	slot = MakeSingleTupleTableSlot(workspace->descriptor,
		&TTSOpsMinimalTuple);
	for (index = 0; index < workspace->visible_rows; index++)
	{
		ExecStoreMinimalTuple(workspace->tuples[index], slot, false);
		if (bloompg_filter_contains_slot(filter->filter, slot,
				filter->attributes))
			matched++;
		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);
	if (fraction != NULL)
		*fraction = (double) matched / (double) workspace->visible_rows;
	if (rows_examined != NULL)
		*rows_examined = workspace->visible_rows;
	if (rows_matched != NULL)
		*rows_matched = matched;
	if (probe_ms != NULL)
		*probe_ms = bloompg_elapsed_ms(started);
	return true;
}

/*
 * Estimate only the incremental effect of filters received since the current
 * materialization was sealed.  This deliberately mirrors DuckDB's CDC path:
 * sample the current intermediate relation, not the original base table.
 *
 * The denominator and numerator come from exactly the same selected chunks.
 * Consequently a filter that rejects no row from the current materialization
 * has an exact sampled fraction of 1.0, independent of base-table sampling
 * error.  Formal compaction remains the correctness boundary.
 */
double
bloompg_sample_materialized_relation(BloomPGColumnStore *store,
	TupleDesc descriptor, List *filters,
	BloomPGIntermediateSamplingProfile *profile)
{
	uint64		exact_rows;
	uint64		examined = 0;
	uint64		passed = 0;
	double		fraction;
	double		estimated_rows;
	instr_time started;
	ListCell   *cell;

	if (store == NULL || descriptor == NULL)
		elog(ERROR, "invalid bloompg intermediate sample source");
	exact_rows = bloompg_column_store_live_rows(store);
	INSTR_TIME_SET_CURRENT(started);
	if (profile != NULL)

	foreach(cell, filters)
	{
		BloomPGSampleFilter *sample_filter = lfirst(cell);

		if (sample_filter->filter == NULL || !sample_filter->filter->valid)
			elog(ERROR, "bloompg intermediate sample found an invalid filter");
		if (sample_filter->filter->inserted == 0)
		{
			fraction = 0.0;
			goto finished;
		}
	}

	if (exact_rows == 0 || filters == NIL)
	{
		fraction = exact_rows == 0 ? 0.0 : 1.0;
		goto finished;
	}

	{
		uint32		chunk_count = bloompg_column_store_chunk_count(store);
		uint32		nonempty_count = 0;
		uint32		target_count;
		uint32	   *nonempty_chunks;
		uint32		chunk_index;
		uint32		sample_index;

		if (chunk_count == 0)
		{
			fraction = 0.0;
			goto finished;
		}
		nonempty_chunks = palloc_array(uint32, chunk_count);
		for (chunk_index = 0; chunk_index < chunk_count; chunk_index++)
		{
			BloomPGColumnStoreChunkView view;

			if (!bloompg_column_store_chunk_view(store, chunk_index, &view))
				elog(ERROR, "bloompg lost an intermediate sample chunk");
			if (view.selected_count > 0)
				nonempty_chunks[nonempty_count++] = chunk_index;
		}
		if (nonempty_count == 0)
		{
			pfree(nonempty_chunks);
			fraction = 0.0;
			goto finished;
		}

		target_count = nonempty_count;
		if (bloompg_sample_rate > 0.0 && bloompg_sample_rate < 1.0)
		{
			double requested = (double) nonempty_count * bloompg_sample_rate;

			target_count = (uint32) requested;
			if ((double) target_count < requested)
				target_count++;
			target_count = Max(target_count, 1U);
			target_count = Min(target_count, nonempty_count);
		}

		for (sample_index = 0; sample_index < target_count; sample_index++)
		{
			uint32 rank = (uint32) (((uint64) sample_index * nonempty_count) /
				target_count);
			BloomPGColumnStoreChunkView view;
			uint32 selected;

			chunk_index = nonempty_chunks[rank];
			if (!bloompg_column_store_chunk_view(store, chunk_index, &view))
				elog(ERROR, "bloompg lost an intermediate sample chunk");
			for (selected = 0; selected < view.selected_count; selected++)
			{
				uint32 row = view.selection[selected];
				bool accepted = true;

				foreach(cell, filters)
				{
					BloomPGSampleFilter *sample_filter = lfirst(cell);

					if (!bloompg_filter_contains_vector(sample_filter->filter,
							&view, row,
							sample_filter->materialized_attributes))
					{
						accepted = false;
						break;
					}
				}
				examined++;
				if (accepted)
					passed++;
			}
			CHECK_FOR_INTERRUPTS();
		}
		pfree(nonempty_chunks);
	}

	fraction = examined > 0 ? (double) passed / (double) examined : 0.0;

finished:
	estimated_rows = exact_rows * fraction;
	if (passed > 0)
		estimated_rows = Max(estimated_rows, 1.0);
	if (profile != NULL)
	{
		profile->calls++;
		profile->rows_examined += examined;
		profile->rows_passed += passed;
		profile->probe_ms += bloompg_elapsed_ms(started);
		profile->last_fraction = fraction;
	}
	return estimated_rows;
}

static bool
bloompg_estimate_from_tid_sample(BloomPGTidSample *sample, Plan *scan,
								 double base_rows, Oid relfilenode,
								 ParamListInfo bound_params,
								 List *filters,
								 BloomPGSampleWorkspace **workspace,
								 double *estimated_rows,
								 double *filter_fraction)
{
	List *quals = bloompg_sampling_safe_quals(bloompg_scan_base_quals(scan));

	if (quals == NIL && filters == NIL)
		return false;
	if (sample->count == 0)
	{
		*estimated_rows = 0.0;
		return true;
	}
	if (*workspace != NULL &&
		((*workspace)->relation_oid != sample->relation_oid ||
		 (*workspace)->relfilenode != relfilenode))
	{
		bloompg_sample_workspace_destroy(*workspace);
		*workspace = NULL;
	}
	if (*workspace == NULL)
		*workspace = bloompg_build_sample_workspace(sample, scan, relfilenode,
			bound_params);
	return bloompg_estimate_from_workspace(*workspace, base_rows, filters,
		estimated_rows, filter_fraction);
}

static bool
bloompg_prepared_tid_estimate(PlannedStmt *candidate, Index rti, Plan *scan,
							  double base_rows, Oid relation_oid,
							  Oid relfilenode, const char *query_string,
							  ParamListInfo bound_params,
							  BloomPGSampleWorkspace **workspace,
							  BloomPGSamplingProfile *profile,
							  double *estimated_rows, bool *cache_hit)
{
	BloomPGTidSample *sample = NULL;
	uint64 repeatable_seed = hash_combine64((uint64) relation_oid,
		hash_combine64((uint64) relfilenode, (uint64) bloompg_sample_seed));
	instr_time acquire_started;
	instr_time build_started;

	INSTR_TIME_SET_CURRENT(acquire_started);
	*cache_hit = false;
	if (bloompg_sample_memory_cache)
		sample = bloompg_find_tid_sample(relation_oid, relfilenode);
	if (sample != NULL)
	{
		*cache_hit = true;
		if (profile != NULL)
			profile->memory_hits++;
	}
	else
	{
		sample = bloompg_load_tid_sample(relation_oid, relfilenode);
		if (sample != NULL)
		{
			*cache_hit = true;
			if (profile != NULL)
				profile->disk_hits++;
			sample = bloompg_remember_tid_sample(sample);
		}
	}
	if (sample == NULL)
	{
		INSTR_TIME_SET_CURRENT(build_started);
		sample = bloompg_build_tid_sample(candidate, rti, scan, base_rows,
			relation_oid, relfilenode, query_string, bound_params, true,
			repeatable_seed);
		bloompg_store_tid_sample(sample);
		sample = bloompg_remember_tid_sample(sample);
		if (profile != NULL)
		{
			profile->builds++;
			profile->build_ms += bloompg_elapsed_ms(build_started);
		}
	}
	if (profile != NULL)
	{
		profile->acquisitions++;
		profile->acquire_ms += bloompg_elapsed_ms(acquire_started);
	}
	return bloompg_estimate_from_tid_sample(sample, scan, base_rows,
		relfilenode, bound_params, NIL, workspace, estimated_rows, NULL);
}

double
bloompg_sample_filtered_relation(PlannedStmt *candidate,
								 Index rti,
								 Plan *scan,
								 double base_rows,
								 List *filters,
								 const char *query_string,
								 ParamListInfo bound_params,
								 BloomPGSampleWorkspace **workspace,
								 BloomPGSamplingProfile *profile,
								 bool *sampled,
								 bool *cache_hit,
								 double *filter_fraction)
{
	RangeTblEntry *rte;
	BloomPGTidSample *sample = NULL;
	Oid relfilenode;
	uint64 repeatable_seed;
	bool prepared;
	double estimated_rows;
	instr_time acquire_started;
	instr_time build_started;

	*sampled = false;
	*cache_hit = false;
	if (filter_fraction != NULL)
		*filter_fraction = 1.0;
	if (filters == NIL || rti == 0 ||
		rti > (Index) list_length(candidate->rtable) || !ActiveSnapshotSet())
		return scan->plan_rows;
	rte = list_nth_node(RangeTblEntry, candidate->rtable, rti - 1);
	if (rte->rtekind != RTE_RELATION ||
		(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW))
		return scan->plan_rows;
	relfilenode = bloompg_relation_filenode(rte->relid);
	repeatable_seed = hash_combine64((uint64) rte->relid,
		hash_combine64((uint64) relfilenode, (uint64) bloompg_sample_seed));
	prepared = pg_strcasecmp(bloompg_sample_mode, "prepared") == 0;
	INSTR_TIME_SET_CURRENT(acquire_started);
	if (prepared && bloompg_sample_memory_cache)
		sample = bloompg_find_tid_sample(rte->relid, relfilenode);
	if (sample != NULL)
	{
		*cache_hit = true;
		if (profile != NULL)
			profile->memory_hits++;
	}
	else if (prepared)
	{
		sample = bloompg_load_tid_sample(rte->relid, relfilenode);
		if (sample != NULL)
		{
			*cache_hit = true;
			if (profile != NULL)
				profile->disk_hits++;
			sample = bloompg_remember_tid_sample(sample);
		}
	}
	if (sample == NULL)
	{
		INSTR_TIME_SET_CURRENT(build_started);
		sample = bloompg_build_tid_sample(candidate, rti, scan, base_rows,
			rte->relid, relfilenode, query_string, bound_params, prepared,
			repeatable_seed);
		if (prepared)
		{
			bloompg_store_tid_sample(sample);
			sample = bloompg_remember_tid_sample(sample);
		}
		if (profile != NULL)
		{
			profile->builds++;
			profile->build_ms += bloompg_elapsed_ms(build_started);
		}
	}
	if (profile != NULL)
	{
		profile->acquisitions++;
		profile->acquire_ms += bloompg_elapsed_ms(acquire_started);
	}
	if (!bloompg_estimate_from_tid_sample(sample, scan, base_rows,
			relfilenode, bound_params, filters, workspace, &estimated_rows,
			filter_fraction))
		estimated_rows = scan->plan_rows;
	else
		*sampled = true;
	if (!prepared)
		bloompg_free_tid_sample(sample);
	return estimated_rows;
}

static SampleScan *
bloompg_make_sample_scan(SeqScan *source, double base_rows, bool prepared,
						 uint64 repeatable_seed)
{
	SampleScan *sample = makeNode(SampleScan);
	Plan *plan = &sample->scan.plan;
	TableSampleClause *clause = makeNode(TableSampleClause);
	double percentage = base_rows > 0 ?
		100.0 * (double) bloompg_sample_size / base_rows : 100.0;
	float4 percentage_float;

	percentage = Max(0.000001, Min(percentage, 100.0));
	percentage_float = (float4) percentage;
	plan->startup_cost = source->scan.plan.startup_cost;
	plan->total_cost = source->scan.plan.total_cost;
	plan->plan_rows = source->scan.plan.plan_rows;
	plan->plan_width = source->scan.plan.plan_width;
	plan->parallel_aware = false;
	plan->parallel_safe = false;
	plan->async_capable = false;
	plan->plan_node_id = source->scan.plan.plan_node_id;
	plan->targetlist = copyObject(source->scan.plan.targetlist);
	plan->qual = copyObject(source->scan.plan.qual);
	plan->initPlan = NIL;
	plan->extParam = bms_copy(source->scan.plan.extParam);
	plan->allParam = bms_copy(source->scan.plan.allParam);
	sample->scan.scanrelid = source->scan.scanrelid;
	clause->tsmhandler = bloompg_system_sample_handler();
	clause->args = list_make1(makeConst(FLOAT4OID, -1, InvalidOid,
										 sizeof(float4),
										 Float4GetDatum(percentage_float),
										 false, true));
	if (prepared)
		clause->repeatable = (Expr *) makeConst(FLOAT8OID, -1, InvalidOid,
												 sizeof(float8),
											 Float8GetDatum((float8) repeatable_seed),
												 false, FLOAT8PASSBYVAL);
	sample->tablesample = clause;
	return sample;
}

double
bloompg_sample_relation(PlannedStmt *candidate,
						Index rti,
						Plan *scan,
						double base_rows,
						const char *query_string,
						ParamListInfo bound_params,
						BloomPGSampleWorkspace **workspace,
						BloomPGSamplingProfile *profile,
						bool *sampled,
						bool *cache_hit)
{
	RangeTblEntry *rte;
	uint64 signature;
	uint64 repeatable_seed;
	Oid relfilenode;
	bool prepared;
	BloomPGPreparedSample *cached;
	PlannedStmt *detached;
	QueryDesc *query_desc = NULL;
	DestReceiver *receiver = NULL;
	volatile double passed = 0;
	volatile double filtered = 0;
	double estimated_rows;
	instr_time sample_started;

	*sampled = false;
	*cache_hit = false;
	if (rti == 0 || rti > (Index) list_length(candidate->rtable) ||
		!ActiveSnapshotSet())
		return scan->plan_rows;
	rte = list_nth_node(RangeTblEntry, candidate->rtable, rti - 1);
	if (rte->rtekind != RTE_RELATION)
		return scan->plan_rows;
	prepared = pg_strcasecmp(bloompg_sample_mode, "prepared") == 0;
	relfilenode = bloompg_relation_filenode(rte->relid);
	repeatable_seed = hash_combine64((uint64) rte->relid,
		hash_combine64((uint64) relfilenode, (uint64) bloompg_sample_seed));
	if (prepared && bloompg_scan_base_quals(scan) != NIL &&
		bloompg_prepared_tid_estimate(candidate, rti, scan, base_rows,
			rte->relid, relfilenode, query_string, bound_params,
			workspace, profile, &estimated_rows, cache_hit))
	{
		*sampled = true;
		return estimated_rows;
	}
	/* If a prepared TID sample cannot be evaluated (for example all cached
	 * TIDs became invisible), use a query-local sample.  Never persist a
	 * predicate-specific estimate under the prepared-sample setting. */
	if (prepared)
		prepared = false;
	if (!IsA(scan, SeqScan) || scan->qual == NIL)
		return scan->plan_rows;
	signature = bloompg_qualifier_signature(scan);
	if (prepared &&
		bloompg_sample_memory_cache &&
		(cached = bloompg_find_prepared_sample(rte->relid, relfilenode,
			signature)) != NULL)
	{
		*sampled = true;
		*cache_hit = true;
		return cached->estimated_rows;
	}
	if (prepared && bloompg_load_disk_sample(rte->relid, relfilenode,
		signature, &estimated_rows))
	{
		if (bloompg_sample_memory_cache)
			bloompg_store_prepared_sample(rte->relid, relfilenode, signature,
				estimated_rows);
		*sampled = true;
		*cache_hit = true;
		return estimated_rows;
	}

	INSTR_TIME_SET_CURRENT(sample_started);
	detached = copyObject(candidate);
	detached->planTree = (Plan *) bloompg_make_sample_scan(castNode(SeqScan, scan),
													 base_rows, prepared,
													 repeatable_seed);
	detached->canSetTag = false;
	detached->parallelModeNeeded = false;
	detached->rowMarks = NIL;
	detached->resultRelations = NIL;
	detached->jitFlags = 0;
	receiver = CreateDestReceiver(DestNone);

	PG_TRY();
	{
		query_desc = CreateQueryDesc(detached,
			query_string != NULL ? query_string : "<bloompg sample>",
			GetActiveSnapshot(), InvalidSnapshot, receiver, bound_params, NULL,
			INSTRUMENT_ROWS);
		ExecutorStart(query_desc, 0);
		ExecutorRun(query_desc, ForwardScanDirection, 0);
		ExecutorFinish(query_desc);
		if (query_desc->planstate->instrument != NULL)
		{
			InstrEndLoop(query_desc->planstate->instrument);
			passed = query_desc->planstate->instrument->ntuples;
			filtered = query_desc->planstate->instrument->nfiltered1;
		}
		ExecutorEnd(query_desc);
		FreeQueryDesc(query_desc);
		query_desc = NULL;
		receiver->rDestroy(receiver);
		receiver = NULL;
	}
	PG_CATCH();
	{
		/* The optimizer subtransaction abort owns failed executor cleanup. */
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (profile != NULL)
	{
		double sample_ms = bloompg_elapsed_ms(sample_started);

		profile->acquisitions++;
		profile->builds++;
		profile->acquire_ms += sample_ms;
		profile->build_ms += sample_ms;
	}

	if (passed + filtered <= 0)
		return scan->plan_rows;
	estimated_rows = base_rows * passed / (passed + filtered);
	if (base_rows <= (double) bloompg_sample_size)
		estimated_rows = passed;
	estimated_rows = Max(estimated_rows, passed > 0 ? 1.0 : 0.0);
	*sampled = true;
	if (prepared)
	{
		if (bloompg_sample_memory_cache)
			bloompg_store_prepared_sample(rte->relid, relfilenode, signature,
				estimated_rows);
		bloompg_store_disk_sample(rte->relid, relfilenode, signature,
			estimated_rows);
	}
	return estimated_rows;
}

void
bloompg_sample_workspace_stats(BloomPGSampleWorkspace *workspace,
								   uint64 *visible_rows,
								   uint64 *local_rows,
								   double *load_ms,
								   double *hydrate_ms,
								   double *qual_ms,
									   bool *tuple_cache_hit,
										   uint64 *probe_count,
								   double *probe_ms,
								   uint64 *probe_rows_examined,
								   uint64 *probe_filter_checks,
								   uint64 *probe_filters_applied,
								   uint64 *probe_cache_hits)
{
	if (visible_rows != NULL)
		*visible_rows = workspace != NULL ? workspace->visible_rows : 0;
	if (local_rows != NULL)
		*local_rows = workspace != NULL ? workspace->local_rows : 0;
	if (load_ms != NULL)
		*load_ms = workspace != NULL ? workspace->load_ms : 0.0;
	if (hydrate_ms != NULL)
		*hydrate_ms = workspace != NULL ? workspace->hydrate_ms : 0.0;
	if (qual_ms != NULL)
		*qual_ms = workspace != NULL ? workspace->qual_ms : 0.0;
	if (tuple_cache_hit != NULL)
		*tuple_cache_hit = workspace != NULL && workspace->tuple_cache_hit;
	if (probe_count != NULL)
		*probe_count = workspace != NULL ? workspace->probe_count : 0;
	if (probe_ms != NULL)
		*probe_ms = workspace != NULL ? workspace->probe_ms : 0.0;
	if (probe_rows_examined != NULL)
		*probe_rows_examined = workspace != NULL ?
			workspace->probe_rows_examined : 0;
	if (probe_filter_checks != NULL)
		*probe_filter_checks = workspace != NULL ?
			workspace->probe_filter_checks : 0;
	if (probe_filters_applied != NULL)
		*probe_filters_applied = workspace != NULL ?
			workspace->probe_filters_applied : 0;
	if (probe_cache_hits != NULL)
		*probe_cache_hits = workspace != NULL ?
			workspace->probe_cache_hits : 0;
}

void
bloompg_sample_workspace_destroy(BloomPGSampleWorkspace *workspace)
{
	MemoryContext context;

	if (workspace == NULL)
		return;
	context = workspace->memory_context;
	MemoryContextDelete(context);
}

void
bloompg_sampling_init(void)
{
	prepared_samples = NIL;
	prepared_tid_samples = NIL;
	prepared_tid_cache_bytes = 0;
	prepared_tuple_cache_bytes = 0;
}

void
bloompg_sampling_fini(void)
{
	ListCell *cell;

	foreach(cell, prepared_samples)
		pfree(lfirst(cell));
	list_free(prepared_samples);
	foreach(cell, prepared_tid_samples)
		bloompg_free_tid_sample(lfirst(cell));
	list_free(prepared_tid_samples);
	prepared_samples = NIL;
	prepared_tid_samples = NIL;
	prepared_tid_cache_bytes = 0;
	prepared_tuple_cache_bytes = 0;
	system_sample_handler = InvalidOid;
	bernoulli_sample_handler = InvalidOid;
}
