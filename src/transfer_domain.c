#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/filter.h"
#include "bloompg/graph.h"
#include "bloompg/transfer_internal.h"

#include "catalog/pg_type_d.h"
#include "utils/typcache.h"

/*
 * Equality domains are an optimization-only summary of exact key filters.
 * The first implementation is deliberately narrow: unprotected inner joins,
 * one canonical equality over the same integral type, and an exact bitmap.
 * Every unsupported shape retains the existing edge-driven propagation.
 *
 * A domain deliberately stores only the exact distinct-key count, not the key
 * set.  Relations remember the domain version they have already consumed.  A
 * source at the current version can only emit a subset of that domain, so an
 * equal count proves that the set did not change; a smaller count advances the
 * version.  Sources outside that containment invariant retain the established
 * edge-driven propagation path.
 */

typedef struct BloomPGEqualityMember
{
	Index		rti;
	AttrNumber	attribute;
	Oid			type_oid;
	Oid			collation;
	int			parent;
} BloomPGEqualityMember;

struct BloomPGEqualityDomain
{
	Oid			type_oid;
	bool		initialized;
	uint64		ndistinct;
	uint64		version;
	uint64	   *applied_versions;
	uint64		last_prepare_epoch;
	uint64		prepared_version;
};

struct BloomPGEqualityDomains
{
	int			relation_count;
	Size		matrix_width;
	BloomPGEqualityDomain **directions;
	List	   *domains;
	uint64		prepare_epoch;
	uint64		tracked;
	uint64		updates;
	uint64		noops;
	uint64		suppressed;
	uint64		observations;
	uint64		uncontained;
	double		update_ms;
};

static bool
bloompg_domain_integral_type(Oid type_oid)
{
	return type_oid == INT2OID || type_oid == INT4OID ||
		type_oid == INT8OID || type_oid == OIDOID || type_oid == DATEOID;
}

static bool
bloompg_domain_edge_eligible(const BloomPGJoinEdge *edge)
{
	TypeCacheEntry *type_cache;

	if (edge->join_type != JOIN_INNER || edge->protect_left ||
		edge->protect_right || edge->left.type_oid != edge->right.type_oid ||
		edge->left.collation != edge->right.collation ||
		!bloompg_domain_integral_type(edge->left.type_oid))
		return false;
	type_cache = lookup_type_cache(edge->left.type_oid, TYPECACHE_EQ_OPR);
	return type_cache != NULL && OidIsValid(type_cache->eq_opr) &&
		type_cache->eq_opr == edge->operator_oid;
}

static int
bloompg_domain_find_member(BloomPGEqualityMember *members, int count,
	const BloomPGColumnRef *column)
{
	int member;

	for (member = 0; member < count; member++)
	{
		if (members[member].rti == column->rti &&
			members[member].attribute == column->attribute &&
			members[member].type_oid == column->type_oid &&
			members[member].collation == column->collation)
			return member;
	}
	return -1;
}

static int
bloompg_domain_add_member(BloomPGEqualityMember *members, int *count,
	const BloomPGColumnRef *column)
{
	int member = bloompg_domain_find_member(members, *count, column);

	if (member >= 0)
		return member;
	member = (*count)++;
	members[member].rti = column->rti;
	members[member].attribute = column->attribute;
	members[member].type_oid = column->type_oid;
	members[member].collation = column->collation;
	members[member].parent = member;
	return member;
}

static int
bloompg_domain_find_root(BloomPGEqualityMember *members, int member)
{
	int root = member;

	while (members[root].parent != root)
		root = members[root].parent;
	while (members[member].parent != member)
	{
		int parent = members[member].parent;

		members[member].parent = root;
		member = parent;
	}
	return root;
}

static void
bloompg_domain_union(BloomPGEqualityMember *members, int left, int right)
{
	int left_root = bloompg_domain_find_root(members, left);
	int right_root = bloompg_domain_find_root(members, right);

	if (left_root != right_root)
		members[right_root].parent = left_root;
}

BloomPGEqualityDomains *
bloompg_equality_domains_create(const BloomPGJoinGraph *graph,
	int relation_count)
{
	BloomPGEqualityDomains *result;
	BloomPGEqualityMember *members;
	BloomPGEqualityDomain **root_domains;
	BloomPGEqualityDomain **first_directions;
	bool *root_valid;
	int *direction_counts;
	Size maximum_members = Max(
		mul_size((Size) list_length(graph->edges), 2), (Size) 1);
	int member_count = 0;
	Size matrix_width = add_size((Size) relation_count, 1);
	Size matrix_cells = mul_size(matrix_width, matrix_width);
	ListCell *cell;
	int member;
	Size direction;

	if (bloompg_transfer_progress_metric != BLOOMPG_TRANSFER_PROGRESS_NDV)
		return NULL;
	result = palloc0(sizeof(BloomPGEqualityDomains));
	result->relation_count = relation_count;
	result->matrix_width = matrix_width;
	result->directions = palloc0_array(BloomPGEqualityDomain *,
		matrix_cells);
	members = palloc0_array(BloomPGEqualityMember, maximum_members);
	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *edge = lfirst(cell);
		int left;
		int right;

		if (!bloompg_domain_edge_eligible(edge))
			continue;
		left = bloompg_domain_add_member(members, &member_count, &edge->left);
		right = bloompg_domain_add_member(members, &member_count, &edge->right);
		bloompg_domain_union(members, left, right);
	}
	root_domains = palloc0_array(BloomPGEqualityDomain *,
		Max(member_count, 1));
	root_valid = palloc_array(bool, Max(member_count, 1));
	for (member = 0; member < member_count; member++)
		root_valid[member] = true;
	/* A relation may contribute only one column to a size-only domain.  With
	 * two columns, consuming a version through one attribute would not prove
	 * containment for a filter later built from the other attribute. */
	for (member = 0; member < member_count; member++)
	{
		int other;
		int root = bloompg_domain_find_root(members, member);

		for (other = 0; other < member; other++)
		{
			if (members[other].rti == members[member].rti &&
				bloompg_domain_find_root(members, other) == root)
			{
				root_valid[root] = false;
				break;
			}
		}
	}
	for (member = 0; member < member_count; member++)
	{
		int root = bloompg_domain_find_root(members, member);
		BloomPGEqualityDomain *domain = root_domains[root];

		if (!root_valid[root])
			continue;
		if (domain == NULL)
		{
			domain = palloc0(sizeof(BloomPGEqualityDomain));
			domain->type_oid = members[member].type_oid;
			domain->applied_versions = palloc0_array(uint64, matrix_width);
			root_domains[root] = domain;
			result->domains = lappend(result->domains, domain);
		}
	}

	/* A transfer with more than one edge between the same pair is composite
	 * and remains on the established edge path. */
	first_directions = palloc0_array(BloomPGEqualityDomain *,
		matrix_cells);
	direction_counts = palloc0_array(int, matrix_cells);
	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *edge = lfirst(cell);
		int left_member;
		int root;
		BloomPGEqualityDomain *domain;
		Size left_offset;
		Size right_offset;

		if (!bloompg_domain_edge_eligible(edge))
			continue;
		left_member = bloompg_domain_find_member(members, member_count,
			&edge->left);
		Assert(left_member >= 0);
		root = bloompg_domain_find_root(members, left_member);
		domain = root_domains[root];
		left_offset = edge->left.rti * matrix_width + edge->right.rti;
		right_offset = edge->right.rti * matrix_width + edge->left.rti;
		if (direction_counts[left_offset]++ == 0)
			first_directions[left_offset] = domain;
		if (direction_counts[right_offset]++ == 0)
			first_directions[right_offset] = domain;
	}
	for (direction = 0; direction < matrix_cells; direction++)
	{
		if (direction_counts[direction] == 1)
			result->directions[direction] = first_directions[direction];
	}
	pfree(direction_counts);
	pfree(first_directions);
	pfree(root_valid);
	pfree(root_domains);
	pfree(members);
	return result;
}

BloomPGEqualityDomain *
bloompg_equality_domain_for_transfer(BloomPGEqualityDomains *domains,
	Index source_rti, Index destination_rti, int nkeys)
{
	if (domains == NULL || nkeys != 1 ||
		source_rti > (Index) domains->relation_count ||
		destination_rti > (Index) domains->relation_count)
		return NULL;
	return domains->directions[source_rti * domains->matrix_width +
		destination_rti];
}

static void
bloompg_domain_observe(BloomPGEqualityDomains *domains,
	BloomPGEqualityDomain *domain, BloomPGRelationState *source,
	BloomPGPendingTransfer *pending)
{
	uint64 ndistinct;

	domain->prepared_version = 0;
	if (!pending->integral_key || pending->filter == NULL ||
		pending->filter->nkeys != 1 ||
		pending->filter->keys[0].build_type != domain->type_oid)
		return;
	if (pending->exact_ndistinct_valid)
		ndistinct = pending->exact_ndistinct;
	else if (!pending->has_range)
		/* An exact integral source with no non-NULL key has an empty domain,
		 * even though the empty filter need not be converted to a bitmap. */
		ndistinct = 0;
	else
		return;

	domains->observations++;
	if (!domain->initialized)
	{
		domain->initialized = true;
		domain->ndistinct = ndistinct;
		domain->version = 1;
		domain->applied_versions[source->rti] = domain->version;
		domain->prepared_version = domain->version;
		domains->tracked++;
		domains->updates++;
		return;
	}

	/* Size alone identifies equality only under containment.  A source that
	 * consumed the current exact domain can only emit a subset.  Sources that
	 * have not consumed it stay on the original edge-driven path. */
	if (domain->applied_versions[source->rti] != domain->version)
	{
		domains->uncontained++;
		return;
	}
	if (ndistinct > domain->ndistinct)
	{
		/* This should be impossible for exact containment.  Preserve the
		 * established transfer rather than trusting a broken invariant. */
		domains->uncontained++;
		return;
	}
	if (ndistinct < domain->ndistinct)
	{
		domain->ndistinct = ndistinct;
		domain->version++;
		domain->applied_versions[source->rti] = domain->version;
		domains->updates++;
	}
	else
		domains->noops++;
	domain->prepared_version = domain->version;
}

void
bloompg_equality_domains_prepare_source(BloomPGEqualityDomains *domains,
	BloomPGRelationState *source, List *pending_transfers)
{
	ListCell *cell;
	instr_time started;

	if (domains == NULL)
		return;
	domains->prepare_epoch++;
	INSTR_TIME_SET_CURRENT(started);
	foreach(cell, pending_transfers)
	{
		BloomPGPendingTransfer *pending = lfirst(cell);
		BloomPGEqualityDomain *domain = pending->equality_domain;

		if (domain == NULL || !pending->build_filter ||
			domain->last_prepare_epoch == domains->prepare_epoch)
			continue;
		domain->last_prepare_epoch = domains->prepare_epoch;
		bloompg_domain_observe(domains, domain, source, pending);
	}
	foreach(cell, pending_transfers)
	{
		BloomPGPendingTransfer *pending = lfirst(cell);

		if (pending->equality_domain != NULL &&
			pending->equality_domain->last_prepare_epoch ==
				domains->prepare_epoch)
			pending->equality_domain_version =
				pending->equality_domain->prepared_version;
	}
	{
		instr_time elapsed;

		INSTR_TIME_SET_CURRENT(elapsed);
		INSTR_TIME_SUBTRACT(elapsed, started);
		domains->update_ms += INSTR_TIME_GET_MILLISEC(elapsed);
	}
}

bool
bloompg_equality_domain_suppresses(BloomPGEqualityDomains *domains,
	BloomPGRelationState *source, BloomPGPendingTransfer *pending)
{
	BloomPGEqualityDomain *domain;
	BloomPGRelationState *destination;

	if (domains == NULL || pending == NULL ||
		pending->equality_domain == NULL)
		return false;
	domain = pending->equality_domain;
	destination = pending->destination;
	if (!domain->initialized || pending->equality_domain_version == 0 ||
		domain->version != pending->equality_domain_version ||
		domain->applied_versions[destination->rti] <
			pending->equality_domain_version)
		return false;
	domains->suppressed++;
	*pending->sent_lineage = bms_union(*pending->sent_lineage,
		source->lineage);
	return true;
}

void
bloompg_equality_domain_mark_applied(BloomPGEqualityDomains *domains,
	BloomPGPendingTransfer *pending, BloomPGRelationState *destination)
{
	BloomPGEqualityDomain *domain;

	if (domains == NULL || pending == NULL || destination == NULL ||
		pending->equality_domain == NULL)
		return;
	domain = pending->equality_domain;
	if (domain->initialized && pending->equality_domain_version > 0 &&
		domain->version == pending->equality_domain_version)
		domain->applied_versions[destination->rti] =
			pending->equality_domain_version;
}

void
bloompg_equality_domains_finish(BloomPGEqualityDomains *domains,
	BloomPGTransferOutcome *outcome, StringInfo detail)
{
	if (domains == NULL)
		return;
	outcome->equality_domain_classes = (uint64) list_length(domains->domains);
	outcome->equality_domain_tracked = domains->tracked;
	outcome->equality_domain_updates = domains->updates;
	outcome->equality_domain_noops = domains->noops;
	outcome->equality_domain_suppressed = domains->suppressed;
	outcome->equality_domain_observations = domains->observations;
	outcome->equality_domain_uncontained = domains->uncontained;
	outcome->equality_domain_ms = domains->update_ms;
	appendStringInfo(detail,
		" domains=" UINT64_FORMAT ":tracked=" UINT64_FORMAT
		":updates=" UINT64_FORMAT ":noops=" UINT64_FORMAT
		":suppressed=" UINT64_FORMAT ":observations=" UINT64_FORMAT
		":uncontained=" UINT64_FORMAT
		":%.3fms",
		outcome->equality_domain_classes,
		outcome->equality_domain_tracked,
		outcome->equality_domain_updates,
		outcome->equality_domain_noops,
		outcome->equality_domain_suppressed,
		outcome->equality_domain_observations,
		outcome->equality_domain_uncontained,
		outcome->equality_domain_ms);
}
