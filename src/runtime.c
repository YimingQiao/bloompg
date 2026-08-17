#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/runtime.h"

#include "access/xact.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

static char *last_trace = NULL;
static List *runtime_filters = NIL;
static List *runtime_materializations = NIL;
static uint64 next_runtime_token = 1;

static void
bloompg_xact_callback(XactEvent event, void *argument)
{
	(void) argument;

	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
			/* Plain EXPLAIN does not start the executor, so its planner-time
			 * materializations have no CustomScan End callback.  Close them
			 * explicitly while TopTransactionContext is still live. */
			bloompg_runtime_cleanup_since(1);
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			runtime_filters = NIL;
			runtime_materializations = NIL;
			break;
		default:
			break;
	}
}

void
bloompg_runtime_init(void)
{
	MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);

	last_trace = pstrdup("no bloompg query has been planned in this backend");
	MemoryContextSwitchTo(old_context);
	RegisterXactCallback(bloompg_xact_callback, NULL);
}

void
bloompg_runtime_fini(void)
{
	if (last_trace != NULL)
	{
		pfree(last_trace);
		last_trace = NULL;
	}
	UnregisterXactCallback(bloompg_xact_callback, NULL);
	runtime_filters = NIL;
	runtime_materializations = NIL;
}

uint64
bloompg_runtime_next_token(void)
{
	uint64 token = next_runtime_token++;

	if (token == 0)
		token = next_runtime_token++;
	return token;
}

uint64
bloompg_snapshot_signature(Snapshot snapshot)
{
	uint64 signature;
	uint32 i;

	if (snapshot == NULL)
		return 0;
	signature = murmurhash64((uint64) snapshot->snapshot_type);
	signature = hash_combine64(signature, (uint64) snapshot->xmin);
	signature = hash_combine64(signature, (uint64) snapshot->xmax);
	signature = hash_combine64(signature, (uint64) snapshot->curcid);
	signature = hash_combine64(signature, snapshot->snapXactCompletionCount);
	signature = hash_combine64(signature, (uint64) snapshot->xcnt);
	for (i = 0; i < snapshot->xcnt; i++)
		signature = hash_combine64(signature, (uint64) snapshot->xip[i]);
	signature = hash_combine64(signature, (uint64) snapshot->subxcnt);
	for (i = 0; i < (uint32) Max(snapshot->subxcnt, 0); i++)
		signature = hash_combine64(signature, (uint64) snapshot->subxip[i]);
	return signature;
}

BloomPGRuntimeFilterEntry *
bloompg_runtime_find_filter(uint64 token, Snapshot snapshot)
{
	uint64 signature = bloompg_snapshot_signature(snapshot);
	ListCell *cell;

	foreach(cell, runtime_filters)
	{
		BloomPGRuntimeFilterEntry *entry = lfirst(cell);

		if (entry->token == token && entry->snapshot_signature == signature)
			return entry;
	}
	return NULL;
}

BloomPGRuntimeFilterEntry *
bloompg_runtime_register_filter(uint64 token,
								Snapshot snapshot,
								BloomPGFilter *filter)
{
	BloomPGRuntimeFilterEntry *entry;
	MemoryContext old_context;
	ListCell *cell;

	foreach(cell, runtime_filters)
	{
		entry = lfirst(cell);
		if (entry->token == token)
		{
			if (entry->filter != filter)
				elog(ERROR, "bloompg runtime filter token was reused");
			entry->snapshot_signature = bloompg_snapshot_signature(snapshot);
			return entry;
		}
	}

	old_context = MemoryContextSwitchTo(TopTransactionContext);
	entry = palloc0(sizeof(BloomPGRuntimeFilterEntry));
	entry->token = token;
	entry->snapshot_signature = bloompg_snapshot_signature(snapshot);
	entry->filter = filter;
	runtime_filters = lappend(runtime_filters, entry);
	MemoryContextSwitchTo(old_context);
	return entry;
}

void
bloompg_runtime_release_filter(uint64 token)
{
	ListCell *cell;

	foreach(cell, runtime_filters)
	{
		BloomPGRuntimeFilterEntry *entry = lfirst(cell);

		if (entry->token == token)
		{
			runtime_filters = list_delete_ptr(runtime_filters, entry);
			bloompg_filter_free(entry->filter);
			pfree(entry);
			return;
		}
	}
}

BloomPGRuntimeMaterializationEntry *
bloompg_runtime_find_materialization(uint64 token, Snapshot snapshot)
{
	uint64 signature = bloompg_snapshot_signature(snapshot);
	ListCell *cell;

	foreach(cell, runtime_materializations)
	{
		BloomPGRuntimeMaterializationEntry *entry = lfirst(cell);

		if (entry->token == token && entry->snapshot_signature == signature)
			return entry;
	}
	return NULL;
}

BloomPGRuntimeMaterializationEntry *
bloompg_runtime_register_materialization(uint64 token,
									 Snapshot snapshot,
									 BloomPGColumnStore *store,
									 TupleDesc descriptor,
									 uint64 rows)
{
	BloomPGRuntimeMaterializationEntry *entry;
	MemoryContext old_context;
	ListCell *cell;

	foreach(cell, runtime_materializations)
	{
		entry = lfirst(cell);
		if (entry->token == token)
		{
			if (entry->store != store ||
				(entry->descriptor != NULL && descriptor != NULL &&
				 entry->descriptor != descriptor))
				elog(ERROR, "bloompg runtime materialization token was reused");
			entry->snapshot_signature = bloompg_snapshot_signature(snapshot);
			if (descriptor != NULL)
				entry->descriptor = descriptor;
			entry->rows = rows;
			return entry;
		}
	}

	old_context = MemoryContextSwitchTo(TopTransactionContext);
	entry = palloc0(sizeof(BloomPGRuntimeMaterializationEntry));
	entry->token = token;
	entry->snapshot_signature = bloompg_snapshot_signature(snapshot);
	entry->store = store;
	entry->descriptor = descriptor;
	entry->rows = rows;
	runtime_materializations = lappend(runtime_materializations, entry);
	MemoryContextSwitchTo(old_context);
	return entry;
}

void
bloompg_runtime_release_materialization(uint64 token)
{
	ListCell *cell;

	foreach(cell, runtime_materializations)
	{
		BloomPGRuntimeMaterializationEntry *entry = lfirst(cell);

		if (entry->token == token)
		{
			if (entry->store != NULL)
			{
				bloompg_column_store_destroy(entry->store);
				entry->store = NULL;
			}
			if (entry->descriptor != NULL)
			{
				FreeTupleDesc(entry->descriptor);
				entry->descriptor = NULL;
			}
			runtime_materializations =
				list_delete_ptr(runtime_materializations, entry);
			pfree(entry);
			return;
		}
	}
}

void
bloompg_runtime_cleanup_since(uint64 first_token)
{
	for (;;)
	{
		BloomPGRuntimeFilterEntry *found = NULL;
		ListCell *cell;

		foreach(cell, runtime_filters)
		{
			BloomPGRuntimeFilterEntry *entry = lfirst(cell);

			if (entry->token >= first_token)
			{
				found = entry;
				break;
			}
		}
		if (found == NULL)
			break;
		bloompg_runtime_release_filter(found->token);
	}
	for (;;)
	{
		BloomPGRuntimeMaterializationEntry *found = NULL;
		ListCell *cell;

		foreach(cell, runtime_materializations)
		{
			BloomPGRuntimeMaterializationEntry *entry = lfirst(cell);

			if (entry->token >= first_token)
			{
				found = entry;
				break;
			}
		}
		if (found == NULL)
			break;
		bloompg_runtime_release_materialization(found->token);
	}
}

const char *
bloompg_get_last_trace(void)
{
	return last_trace != NULL ? last_trace : "";
}

void
bloompg_set_last_trace(const char *trace)
{
	MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);

	if (last_trace != NULL)
		pfree(last_trace);
	last_trace = pstrdup(trace != NULL ? trace : "");
	MemoryContextSwitchTo(old_context);
}
