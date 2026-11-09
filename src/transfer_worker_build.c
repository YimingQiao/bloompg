#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/worker_build.h"

#include "catalog/pg_proc.h"
#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

/* Private filter arrays are bounded separately from vector transport. */
#define BLOOMPG_WORKER_BITMAP_BYTES ((Size) 8 * 1024 * 1024)

/* Summaries travel through the same Gather as row vectors, but never enter
 * the materialized store. Bound each packet even for a large Bloom filter. */
#define FILTER_PACKET_MAGIC 0x42504746U
#define FILTER_PACKET_WORDS 8192

typedef struct FilterPacket
{
	uint32 magic;
	uint32 index;
	uint32 count;
	bool integral;
	bool abandoned;
	bool has_range;
	uint64 offset;
	uint64 words;
	uint64 lower_word;
	uint64 inserted;
	int64 minimum;
	int64 maximum;
	int hashes;
	double build_ms;
} FilterPacket;

List *
bloompg_worker_build_metadata(List *pending, int workers,
	BloomPGMemoryBudget *budget, Size *reserved)
{
	List *metadata = NIL;
	ListCell *cell;
	Size private_bytes = 0;
	int index = 0;

	*reserved = 0;
	if (!bloompg_parallel_filter_build)
		return NIL;
	foreach(cell, pending)
	{
		BloomPGPendingTransfer *p = lfirst(cell);
		List *keys = NIL;
		List *entry;
		int k;

		if (!p->build_filter)
		{
			index++;
			continue;
		}
		for (k = 0; k < p->spec.nkeys; k++)
		{
			if (!p->integral_key &&
				(!OidIsValid(p->filter->keys[k].build_hash_proc) ||
				 func_parallel(p->filter->keys[k].build_hash_proc) !=
				 PROPARALLEL_SAFE))
				return NIL;
			keys = lappend(keys, list_make3(
				makeString(psprintf("%u", p->spec.operators[k])),
				makeString(psprintf("%u", p->spec.collations[k])),
				makeInteger(p->spec.build_attributes[k])));
		}
		entry = list_make5(makeInteger(index++),
			makeString(psprintf(INT64_FORMAT, p->spec.estimated_rows)),
			makeInteger(p->spec.build_is_left), makeInteger(p->integral_key),
			keys);
		metadata = lappend(metadata, entry);
		private_bytes = add_size(private_bytes, p->filter->nbits / 8);
		/* During bitmap growth both old and new arrays coexist. */
		if (p->integral_key)
			private_bytes = add_size(private_bytes, 2 *
				Min(BLOOMPG_WORKER_BITMAP_BYTES, (Size) bloompg_max_filter_bytes));
	}
	if (metadata == NIL)
		return NIL;
	/* Include leader participation, packet copies/queues, and small metadata. */
	private_bytes = add_size(private_bytes, (Size) 1024 * 1024);
	private_bytes = mul_size(private_bytes, (Size) workers + 1);
	if (!bloompg_memory_budget_reserve(budget, private_bytes))
		return NIL;
	*reserved = private_bytes;
	return metadata;
}

List *
bloompg_worker_build_begin(List *metadata)
{
	List *pending = NIL;
	ListCell *cell;

	foreach(cell, metadata)
	{
		List *entry = lfirst(cell);
		List *keys = list_nth(entry, 4);
		ListCell *key_cell;
		BloomPGPendingTransfer *p = palloc0(sizeof(*p));
		int k = 0;

		p->spec.estimated_rows = strtoll(strVal(list_nth(entry, 1)), NULL, 10);
		p->spec.build_is_left = intVal(list_nth(entry, 2));
		p->integral_key = intVal(list_nth(entry, 3));
		p->spec.nkeys = list_length(keys);
		p->spec.operators = palloc_array(Oid, p->spec.nkeys);
		p->spec.collations = palloc_array(Oid, p->spec.nkeys);
		p->spec.build_attributes = palloc_array(AttrNumber, p->spec.nkeys);
		foreach(key_cell, keys)
		{
			List *key = lfirst(key_cell);

			p->spec.operators[k] = strtoul(strVal(linitial(key)), NULL, 10);
			p->spec.collations[k] = strtoul(strVal(lsecond(key)), NULL, 10);
			p->spec.build_attributes[k++] = intVal(lthird(key));
		}
		/* Use the global estimate, not a worker's share: all Bloom layouts and
		 * hash counts must be identical to the leader's filter. */
		p->filter = bloompg_filter_create_bloom(CurrentMemoryContext,
			p->spec.estimated_rows, p->spec.nkeys, p->spec.operators,
			p->spec.collations, p->spec.build_is_left);
		p->build_filter = true;
		p->bitmap_candidate_max_bytes = Min(BLOOMPG_WORKER_BITMAP_BYTES,
			(Size) bloompg_max_filter_bytes);
		pending = lappend(pending, p);
	}
	return pending;
}

bytea *
bloompg_worker_build_packet(List *pending, List *metadata,
	int *index, uint64 *offset, double build_ms)
{
	BloomPGPendingTransfer *p;
	FilterPacket header = {0};
	uint64 *words;
	bytea *packet;
	Size bytes;

	if (*index >= list_length(pending))
		return NULL;
	p = list_nth(pending, *index);
	header.magic = FILTER_PACKET_MAGIC;
	header.index = intVal(linitial((List *) list_nth(metadata, *index)));
	header.integral = p->integral_key;
	header.abandoned = p->bitmap_candidate_abandoned;
	header.has_range = p->has_range;
	header.minimum = p->minimum;
	header.maximum = p->maximum;
	header.hashes = p->filter->hash_functions;
	if (p->integral_key)
	{
		words = p->bitmap_candidate_words;
		header.words = words == NULL ? 0 :
			p->bitmap_candidate_upper_word - p->bitmap_candidate_lower_word + 1;
		header.lower_word = p->bitmap_candidate_lower_word;
		header.inserted = p->bitmap_candidate_inserted;
	}
	else
	{
		words = p->filter->words;
		header.words = p->filter->nbits / 64;
		header.inserted = p->filter->inserted;
	}
	header.offset = *offset;
	header.count = Min(header.words - *offset, FILTER_PACKET_WORDS);
	if (*index == 0 && *offset == 0)
		header.build_ms = build_ms;
	bytes = header.count * sizeof(uint64);
	packet = palloc(VARHDRSZ + sizeof(header) + bytes);
	SET_VARSIZE(packet, VARHDRSZ + sizeof(header) + bytes);
	memcpy(VARDATA(packet), &header, sizeof(header));
	if (bytes > 0)
		memcpy(VARDATA(packet) + sizeof(header), words + *offset, bytes);
	*offset += header.count;
	if (*offset == header.words)
	{
		(*index)++;
		*offset = 0;
	}
	return packet;
}

/* Match whole word ranges, including signed values crossing zero. Never OR
 * differently based bitmaps without shifting their word offsets. */
static void
merge_bitmap_range(BloomPGPendingTransfer *p, const FilterPacket *h)
{
	uint64 lower = h->lower_word;
	uint64 upper = lower + h->words - 1;
	uint64 count;
	uint64 *words;

	if (p->bitmap_candidate_words != NULL)
	{
		lower = Min(lower, p->bitmap_candidate_lower_word);
		upper = Max(upper, p->bitmap_candidate_upper_word);
		if (lower == p->bitmap_candidate_lower_word &&
			upper == p->bitmap_candidate_upper_word)
			return;
	}
	count = upper - lower + 1;
	if (count > (uint64) bloompg_max_filter_bytes / sizeof(uint64))
	{
		p->bitmap_candidate_abandoned = true;
		return;
	}
	words = MemoryContextAllocZero(p->filter->memory_context,
		(Size) count * sizeof(uint64));
	if (p->bitmap_candidate_words != NULL)
	{
		memcpy(words + p->bitmap_candidate_lower_word - lower,
			p->bitmap_candidate_words,
			(p->bitmap_candidate_upper_word - p->bitmap_candidate_lower_word + 1) *
			 sizeof(uint64));
		pfree(p->bitmap_candidate_words);
	}
	p->bitmap_candidate_words = words;
	p->bitmap_candidate_lower_word = lower;
	p->bitmap_candidate_upper_word = upper;
}

bool
bloompg_worker_build_receive(Datum datum, List *pending,
	BloomPGRelationState *state)
{
	struct varlena *packet = PG_DETOAST_DATUM_PACKED(datum);
	const char *base = VARDATA_ANY(packet);
	Size size = VARSIZE_ANY_EXHDR(packet);
	FilterPacket h;
	uint32 magic = 0;
	BloomPGPendingTransfer *p;
	uint64 *target = NULL;
	uint32 i;
	instr_time started;

	if (size >= sizeof(magic))
		memcpy(&magic, base, sizeof(magic));
	if (magic != FILTER_PACKET_MAGIC)
	{
		if ((Pointer) packet != DatumGetPointer(datum))
			pfree(packet);
		return false;
	}
	INSTR_TIME_SET_CURRENT(started);
	if (size < sizeof(h))
		elog(ERROR, "truncated bloompg worker filter packet");
	memcpy(&h, base, sizeof(h));
	if (h.index >= (uint32) list_length(pending) ||
		h.count > FILTER_PACKET_WORDS || h.offset > h.words ||
		h.count > h.words - h.offset ||
		size != sizeof(h) + h.count * sizeof(uint64) ||
		h.words > (uint64) bloompg_max_filter_bytes / sizeof(uint64) ||
		h.lower_word > (UINT64_MAX >> 6) ||
		(h.words > 0 && h.words - 1 > (UINT64_MAX >> 6) - h.lower_word))
		elog(ERROR, "invalid bloompg worker filter packet");
	p = list_nth(pending, h.index);
	if (!p->build_filter || h.integral != p->integral_key ||
		(!h.integral && (h.words != p->filter->nbits / 64 ||
		 h.hashes != p->filter->hash_functions)))
		elog(ERROR, "incompatible bloompg worker filter layout");
	if (h.offset == 0)
	{
		state->worker_filter_parts++;
		state->worker_filter_build_ms += h.build_ms;
		if (h.integral)
		{
			if (h.has_range)
			{
				p->minimum = p->has_range ? Min(p->minimum, h.minimum) : h.minimum;
				p->maximum = p->has_range ? Max(p->maximum, h.maximum) : h.maximum;
				p->has_range = true;
			}
			p->bitmap_candidate_inserted += h.inserted;
			p->bitmap_candidate_abandoned |= h.abandoned;
		}
		else
			p->filter->inserted += h.inserted;
	}
	if (!h.integral)
		target = p->filter->words + h.offset;
	else if (!p->bitmap_candidate_abandoned && h.words > 0)
	{
		merge_bitmap_range(p, &h);
		if (!p->bitmap_candidate_abandoned)
			target = p->bitmap_candidate_words +
				h.lower_word - p->bitmap_candidate_lower_word + h.offset;
	}
	if (p->bitmap_candidate_abandoned && p->bitmap_candidate_words != NULL)
	{
		pfree(p->bitmap_candidate_words);
		p->bitmap_candidate_words = NULL;
	}
	if (target != NULL)
		for (i = 0; i < h.count; i++)
		{
			uint64 word;

			/* Gather bytea payloads need not be uint64-aligned. */
			memcpy(&word, base + sizeof(h) + i * sizeof(word), sizeof(word));
			target[i] |= word;
		}
	state->worker_filter_bytes += size;
	state->worker_filter_merge_ms += bloompg_transfer_elapsed_ms(started);
	if ((Pointer) packet != DatumGetPointer(datum))
		pfree(packet);
	return true;
}
