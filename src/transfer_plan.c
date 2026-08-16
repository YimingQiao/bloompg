#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/scan.h"
#include "bloompg/filter.h"
#include "bloompg/graph.h"
#include "bloompg/runtime.h"
#include "bloompg/sampling.h"
#include "bloompg/transfer.h"
#include "bloompg/transfer_internal.h"

#include "catalog/pg_class.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_type_d.h"
#include "access/tableam.h"
#include "access/sysattr.h"
#include "executor/executor.h"
#include "executor/execdesc.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "tcop/dest.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/date.h"
#include "utils/typcache.h"

/* Plan-tree discovery, projection pruning, and replacement rewrites. */

static bool
bloompg_is_supported_scan(const Plan *plan, Index *rti)
{
	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_CteScan:
			*rti = ((const Scan *) plan)->scanrelid;
			return true;
		default:
			return false;
	}
}

Plan *
bloompg_find_scan(Plan *plan, Index wanted_rti)
{
	Index rti;
	Plan *found;
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return NULL;
	if (IsA(plan, Append) &&
		bms_is_member(wanted_rti, castNode(Append, plan)->apprelids))
		return plan;
	if (IsA(plan, MergeAppend) &&
		bms_is_member(wanted_rti, castNode(MergeAppend, plan)->apprelids))
		return plan;
	if (bloompg_is_supported_scan(plan, &rti) && rti == wanted_rti)
		return plan;
	if ((found = bloompg_find_scan(outerPlan(plan), wanted_rti)) != NULL)
		return found;
	if ((found = bloompg_find_scan(innerPlan(plan), wanted_rti)) != NULL)
		return found;

	switch (nodeTag(plan))
	{
		case T_Append:
			children = castNode(Append, plan)->appendplans;
			break;
		case T_MergeAppend:
			children = castNode(MergeAppend, plan)->mergeplans;
			break;
		case T_BitmapAnd:
			children = castNode(BitmapAnd, plan)->bitmapplans;
			break;
		case T_BitmapOr:
			children = castNode(BitmapOr, plan)->bitmapplans;
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		case T_SubqueryScan:
			return bloompg_find_scan(castNode(SubqueryScan, plan)->subplan,
									 wanted_rti);
		default:
			break;
	}
	foreach(cell, children)
	{
		found = bloompg_find_scan((Plan *) lfirst(cell), wanted_rti);
		if (found != NULL)
			return found;
	}
	return NULL;
}

static void
bloompg_collect_parameterized_indexes(Plan *plan, Bitmapset **relids)
{
	List	   *children = NIL;
	ListCell   *cell;
	Index		rti = 0;

	if (plan == NULL)
		return;
	if ((IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) ||
		 IsA(plan, BitmapHeapScan)) && !bms_is_empty(plan->extParam))
		rti = ((Scan *) plan)->scanrelid;
	if (rti > 0)
		*relids = bms_add_member(*relids, rti);
	bloompg_collect_parameterized_indexes(outerPlan(plan), relids);
	bloompg_collect_parameterized_indexes(innerPlan(plan), relids);
	switch (nodeTag(plan))
	{
		case T_Append:
			children = castNode(Append, plan)->appendplans;
			break;
		case T_MergeAppend:
			children = castNode(MergeAppend, plan)->mergeplans;
			break;
		case T_BitmapAnd:
			children = castNode(BitmapAnd, plan)->bitmapplans;
			break;
		case T_BitmapOr:
			children = castNode(BitmapOr, plan)->bitmapplans;
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		case T_SubqueryScan:
			bloompg_collect_parameterized_indexes(
				castNode(SubqueryScan, plan)->subplan, relids);
			break;
		default:
			break;
	}
	foreach(cell, children)
		bloompg_collect_parameterized_indexes((Plan *) lfirst(cell), relids);
}

Bitmapset *
bloompg_parameterized_index_relids(PlannedStmt *statement)
{
	Bitmapset  *result = NULL;
	ListCell   *cell;

	if (statement == NULL)
		return NULL;
	bloompg_collect_parameterized_indexes(statement->planTree, &result);
	foreach(cell, statement->subplans)
		bloompg_collect_parameterized_indexes((Plan *) lfirst(cell), &result);
	return result;
}

static Index
bloompg_plan_scanrelid(Plan *plan)
{
	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapIndexScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_NamedTuplestoreScan:
		case T_WorkTableScan:
		case T_ForeignScan:
			return ((Scan *) plan)->scanrelid;
		case T_CustomScan:
			return castNode(CustomScan, plan)->scan.scanrelid;
		default:
			return 0;
	}
}

Bitmapset *
bloompg_plan_relids(Plan *plan)
{
	Bitmapset *result = NULL;
	List *children = NIL;
	ListCell *cell;
	Index scanrelid;

	if (plan == NULL)
		return NULL;
	scanrelid = bloompg_plan_scanrelid(plan);
	if (scanrelid > 0)
		result = bms_add_member(result, scanrelid);
	result = bms_join(result, bloompg_plan_relids(outerPlan(plan)));
	result = bms_join(result, bloompg_plan_relids(innerPlan(plan)));
	switch (nodeTag(plan))
	{
		case T_Append:
			children = castNode(Append, plan)->appendplans;
			result = bms_join(result,
				bms_copy(castNode(Append, plan)->apprelids));
			break;
		case T_MergeAppend:
			children = castNode(MergeAppend, plan)->mergeplans;
			result = bms_join(result,
				bms_copy(castNode(MergeAppend, plan)->apprelids));
			break;
		case T_BitmapAnd:
			children = castNode(BitmapAnd, plan)->bitmapplans;
			break;
		case T_BitmapOr:
			children = castNode(BitmapOr, plan)->bitmapplans;
			break;
		case T_SubqueryScan:
			result = bms_join(result, bloompg_plan_relids(
				castNode(SubqueryScan, plan)->subplan));
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		default:
			break;
	}
	foreach(cell, children)
		result = bms_join(result,
			bloompg_plan_relids((Plan *) lfirst(cell)));
	return result;
}

/* Find the highest contiguous plan subtree whose scans all belong to one
 * subquery scope.  If pull-up interleaved that scope with another relation,
 * the search returns NULL and Bloom leaves the subquery untouched. */
Plan *
bloompg_find_scope_root(Plan *plan, const Bitmapset *scope_relids)
{
	Bitmapset *relids;
	Plan *found = NULL;
	Plan *candidate;
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return NULL;
	relids = bloompg_plan_relids(plan);
	if (!bms_is_empty(relids) && bms_is_subset(relids, scope_relids) &&
		nodeTag(plan) != T_Hash && nodeTag(plan) != T_BitmapAnd &&
		nodeTag(plan) != T_BitmapOr && nodeTag(plan) != T_BitmapIndexScan)
	{
		bms_free(relids);
		return plan;
	}
	bms_free(relids);

	candidate = bloompg_find_scope_root(outerPlan(plan), scope_relids);
	if (candidate != NULL)
		found = candidate;
	candidate = bloompg_find_scope_root(innerPlan(plan), scope_relids);
	if (candidate != NULL)
	{
		if (found != NULL && found != candidate)
			return NULL;
		found = candidate;
	}
	switch (nodeTag(plan))
	{
		case T_Append:
			children = castNode(Append, plan)->appendplans;
			break;
		case T_MergeAppend:
			children = castNode(MergeAppend, plan)->mergeplans;
			break;
		case T_SubqueryScan:
			candidate = bloompg_find_scope_root(
				castNode(SubqueryScan, plan)->subplan, scope_relids);
			if (candidate != NULL)
			{
				if (found != NULL && found != candidate)
					return NULL;
				found = candidate;
			}
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		default:
			break;
	}
	foreach(cell, children)
	{
		candidate = bloompg_find_scope_root((Plan *) lfirst(cell),
			scope_relids);
		if (candidate != NULL)
		{
			if (found != NULL && found != candidate)
				return NULL;
			found = candidate;
		}
	}
	return found;
}

static bool
bloompg_rte_identity_matches(RangeTblEntry *left, RangeTblEntry *right)
{
	const char *left_alias;
	const char *right_alias;

	if (left->rtekind != right->rtekind || left->rtekind != RTE_RELATION ||
		left->relid != right->relid)
		return false;
	left_alias = left->eref != NULL ? left->eref->aliasname : NULL;
	right_alias = right->eref != NULL ? right->eref->aliasname : NULL;
	return left_alias == NULL || right_alias == NULL ||
		strcmp(left_alias, right_alias) == 0;
}

bool
bloompg_map_subquery(PlannedStmt *candidate, BloomPGJoinGraph *graph,
					 BloomPGRelationState *state, Bitmapset *main_relids,
					 Bitmapset **used_relids)
{
	Query *subquery = state->table->subquery;
	ListCell *cell;
	int local_rti = 0;

	if (subquery == NULL)
		return false;
	state->local_rtable_count = list_length(subquery->rtable);
	state->local_to_global = palloc0_array(Index,
		state->local_rtable_count + 1);
	foreach(cell, subquery->rtable)
	{
		RangeTblEntry *local_rte = lfirst_node(RangeTblEntry, cell);
		int candidate_rti;
		bool matched = false;

		local_rti++;
		if (local_rte->rtekind == RTE_JOIN ||
			local_rte->rtekind == RTE_GROUP ||
			local_rte->rtekind == RTE_RESULT)
			continue;
		/* Nested subquery/CTE output mapping is a separate scope.  Keep the
		 * first implementation fail-closed until that scope can be represented
		 * without relying on planner RTI allocation order. */
		if (local_rte->rtekind != RTE_RELATION)
			return false;
		for (candidate_rti = graph->rtable_count + 1;
			 candidate_rti <= list_length(candidate->rtable); candidate_rti++)
		{
			RangeTblEntry *candidate_rte;

			if (!bms_is_member(candidate_rti, main_relids) ||
				bms_is_member(candidate_rti, *used_relids))
				continue;
			candidate_rte = list_nth_node(RangeTblEntry,
				candidate->rtable, candidate_rti - 1);
			if (!bloompg_rte_identity_matches(local_rte, candidate_rte))
				continue;
			state->local_to_global[local_rti] = candidate_rti;
			state->scope_relids = bms_add_member(state->scope_relids,
				candidate_rti);
			*used_relids = bms_add_member(*used_relids, candidate_rti);
			matched = true;
			break;
		}
		if (!matched)
			return false;
	}
	state->scope_relids = bms_add_member(state->scope_relids, state->rti);
	return !bms_is_empty(state->scope_relids);
}

bool
bloompg_simple_subquery(Query *query)
{
	RangeTblEntry *rte;
	ListCell *cell;

	if (query == NULL || query->hasAggs || query->hasTargetSRFs ||
		query->groupClause != NIL || query->groupingSets != NIL ||
		query->havingQual != NULL || query->windowClause != NIL ||
		query->distinctClause != NIL || query->sortClause != NIL ||
		query->limitOffset != NULL || query->limitCount != NULL ||
		query->setOperations != NULL || query->cteList != NIL ||
		list_length(query->rtable) != 1 ||
		query->jointree == NULL || list_length(query->jointree->fromlist) != 1 ||
		!IsA(linitial(query->jointree->fromlist), RangeTblRef))
		return false;
	rte = linitial_node(RangeTblEntry, query->rtable);
	if (rte->rtekind != RTE_RELATION)
		return false;
	foreach(cell, query->targetList)
	{
		TargetEntry *entry = lfirst_node(TargetEntry, cell);

		if (!entry->resjunk &&
			!IsA(strip_implicit_coercions((Node *) entry->expr), Var))
			return false;
	}
	return true;
}

bool
bloompg_plan_has_parallel_node(Plan *plan)
{
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return false;
	if (IsA(plan, Gather) || IsA(plan, GatherMerge))
		return true;
	if (bloompg_plan_has_parallel_node(outerPlan(plan)) ||
		bloompg_plan_has_parallel_node(innerPlan(plan)))
		return true;
	switch (nodeTag(plan))
	{
		case T_Append:
			children = castNode(Append, plan)->appendplans;
			break;
		case T_MergeAppend:
			children = castNode(MergeAppend, plan)->mergeplans;
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		default:
			break;
	}
	foreach(cell, children)
	{
		if (bloompg_plan_has_parallel_node((Plan *) lfirst(cell)))
			return true;
	}
	return false;
}

int
bloompg_max_plan_node_id(Plan *plan)
{
	int maximum;
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return 0;
	maximum = plan->plan_node_id;
	maximum = Max(maximum, bloompg_max_plan_node_id(outerPlan(plan)));
	maximum = Max(maximum, bloompg_max_plan_node_id(innerPlan(plan)));
	switch (nodeTag(plan))
	{
		case T_Append:
			children = castNode(Append, plan)->appendplans;
			break;
		case T_MergeAppend:
			children = castNode(MergeAppend, plan)->mergeplans;
			break;
		case T_BitmapAnd:
			children = castNode(BitmapAnd, plan)->bitmapplans;
			break;
		case T_BitmapOr:
			children = castNode(BitmapOr, plan)->bitmapplans;
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		case T_SubqueryScan:
			maximum = Max(maximum,
						  bloompg_max_plan_node_id(castNode(SubqueryScan, plan)->subplan));
			break;
		default:
			break;
	}
	foreach(cell, children)
		maximum = Max(maximum,
					  bloompg_max_plan_node_id((Plan *) lfirst(cell)));
	return maximum;
}

/* Replace one relation-operator root, rather than a scan by RTI.  This keeps
 * already-installed filters outside newer filters and makes the state pointer
 * name the exact subtree that will be executed in the next excitation round.
 */
Plan *
bloompg_replace_plan_pointer(Plan *plan, Plan *wanted, Plan *replacement,
							 bool *done)
{
	ListCell *cell;

	if (plan == NULL || *done)
		return plan;
	if (plan == wanted)
	{
		*done = true;
		return replacement;
	}
	plan->lefttree = bloompg_replace_plan_pointer(plan->lefttree, wanted,
											 replacement, done);
	plan->righttree = bloompg_replace_plan_pointer(plan->righttree, wanted,
											  replacement, done);
	if (*done)
		return plan;
	switch (nodeTag(plan))
	{
		case T_Append:
			foreach(cell, castNode(Append, plan)->appendplans)
			{
				lfirst(cell) = bloompg_replace_plan_pointer((Plan *) lfirst(cell),
														 wanted, replacement, done);
				if (*done)
					break;
			}
			break;
		case T_MergeAppend:
			foreach(cell, castNode(MergeAppend, plan)->mergeplans)
			{
				lfirst(cell) = bloompg_replace_plan_pointer((Plan *) lfirst(cell),
														 wanted, replacement, done);
				if (*done)
					break;
			}
			break;
		case T_SubqueryScan:
			castNode(SubqueryScan, plan)->subplan =
				bloompg_replace_plan_pointer(castNode(SubqueryScan, plan)->subplan,
											 wanted, replacement, done);
			break;
		case T_CustomScan:
			foreach(cell, castNode(CustomScan, plan)->custom_plans)
			{
				lfirst(cell) = bloompg_replace_plan_pointer((Plan *) lfirst(cell),
														 wanted, replacement, done);
				if (*done)
					break;
			}
			break;
		default:
			break;
	}
	return plan;
}

static AttrNumber
bloompg_find_output_attribute(Plan *scan, BloomPGColumnRef column)
{
	ListCell *cell;

	foreach(cell, scan->targetlist)
	{
		TargetEntry *entry = lfirst_node(TargetEntry, cell);
		Node *expression = strip_implicit_coercions((Node *) entry->expr);

		if (IsA(expression, Var))
		{
			Var *var = castNode(Var, expression);

			if (((Index) var->varno == column.rti &&
				 var->varattno == column.attribute) ||
				((Index) var->varnosyn == column.rti &&
				 var->varattnosyn == column.attribute))
				return entry->resno;
		}
	}
	if (IsA(scan, CustomScan) &&
		castNode(CustomScan, scan)->custom_plans != NIL)
		return bloompg_find_output_attribute((Plan *)
			linitial(castNode(CustomScan, scan)->custom_plans), column);
	return InvalidAttrNumber;
}

List *
bloompg_semantic_targetlist(Plan *plan)
{
	if (IsA(plan, CustomScan) &&
		castNode(CustomScan, plan)->custom_scan_tlist != NIL)
		return castNode(CustomScan, plan)->custom_scan_tlist;
	return plan->targetlist;
}

AttrNumber
bloompg_prepare_tid_output(Plan *plan, Index rti,
						   const BloomPGJoinGraph *graph)
{
	List *original_targetlist;
	List *narrow_targetlist = NIL;
	AttrNumber next_resno = 1;
	ListCell *cell;

	/* Heap-backed scans expose the table CTID through their scan tuple.  An
	 * IndexOnlyScan does not necessarily visit the heap, so it must stay on
	 * MEMORY unless PostgreSQL already chose another heap-backed scan. */
	if (!IsA(plan, SeqScan) && !IsA(plan, IndexScan) &&
		!IsA(plan, BitmapHeapScan) && !IsA(plan, TidScan) &&
		!IsA(plan, TidRangeScan))
		return InvalidAttrNumber;
	original_targetlist = plan->targetlist;
	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *edge = lfirst(cell);
		BloomPGColumnRef column;
		AttrNumber old_attribute;
		TargetEntry *source_entry = NULL;
		ListCell *target_cell;
		bool duplicate = false;

		if (edge->left.rti == rti)
			column = edge->left;
		else if (edge->right.rti == rti)
			column = edge->right;
		else
			continue;
		old_attribute = bloompg_find_output_attribute(plan, column);
		if (!AttributeNumberIsValid(old_attribute))
			return InvalidAttrNumber;
		foreach(target_cell, original_targetlist)
		{
			TargetEntry *entry = lfirst_node(TargetEntry, target_cell);

			if (entry->resno == old_attribute)
			{
				source_entry = entry;
				break;
			}
		}
		if (source_entry == NULL)
			return InvalidAttrNumber;
		foreach(target_cell, narrow_targetlist)
		{
			TargetEntry *entry = lfirst_node(TargetEntry, target_cell);

			if (equal(entry->expr, source_entry->expr))
			{
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
		{
			TargetEntry *entry = copyObject(source_entry);

			entry->resno = next_resno++;
			entry->resjunk = false;
			narrow_targetlist = lappend(narrow_targetlist, entry);
		}
	}
	if (narrow_targetlist == NIL)
		return InvalidAttrNumber;
	plan->targetlist = lappend(narrow_targetlist,
		makeTargetEntry((Expr *) makeVar(rti, SelfItemPointerAttributeNumber,
			TIDOID, -1, InvalidOid, 0), next_resno, "bloompg_ctid", false));
	return next_resno;
}

/* Core base scans often expose every heap attribute even though upper plan
 * nodes only need a small query-specific subset.  Detached optimizer-time
 * scans do not need to preserve those physical output positions: outgoing
 * filter mappings are rebuilt after this rewrite, and P1 maps expressions by
 * identity.  Keep every query Var (including local-qual columns) plus all
 * graph keys, but fail closed for whole-row/system-attribute references. */
bool
bloompg_prepare_projected_output(Plan *plan, BloomPGTable *table,
	const BloomPGJoinGraph *graph, int *original_columns,
	int *projected_columns)
{
	Bitmapset *required;
	List *original_targetlist;
	List *narrow_targetlist = NIL;
	ListCell *cell;
	int member = -1;
	AttrNumber next_resno = 1;

	*original_columns = list_length(plan->targetlist);
	*projected_columns = *original_columns;
	if (table->required_attributes == NULL ||
		(!IsA(plan, SeqScan) && !IsA(plan, IndexScan) &&
		 !IsA(plan, BitmapHeapScan) && !IsA(plan, TidScan) &&
		 !IsA(plan, TidRangeScan)))
		return false;
	required = bms_copy(table->required_attributes);
	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *edge = lfirst(cell);
		AttrNumber attribute = InvalidAttrNumber;

		if (edge->left.rti == table->rti)
			attribute = edge->left.attribute;
		else if (edge->right.rti == table->rti)
			attribute = edge->right.attribute;
		if (AttributeNumberIsValid(attribute))
			required = bms_add_member(required,
				attribute - FirstLowInvalidHeapAttributeNumber);
	}
	original_targetlist = plan->targetlist;
	while ((member = bms_next_member(required, member)) >= 0)
	{
		AttrNumber attribute = member + FirstLowInvalidHeapAttributeNumber;
		BloomPGColumnRef column;
		AttrNumber old_attribute;
		TargetEntry *source_entry = NULL;
		ListCell *target_cell;

		if (!AttributeNumberIsValid(attribute))
			return false;
		MemSet(&column, 0, sizeof(column));
		column.rti = table->rti;
		column.attribute = attribute;
		old_attribute = bloompg_find_output_attribute(plan, column);
		if (!AttributeNumberIsValid(old_attribute))
			return false;
		foreach(target_cell, original_targetlist)
		{
			TargetEntry *entry = lfirst_node(TargetEntry, target_cell);

			if (entry->resno == old_attribute)
			{
				source_entry = entry;
				break;
			}
		}
		if (source_entry == NULL)
			return false;
		{
			TargetEntry *entry = copyObject(source_entry);

			entry->resno = next_resno++;
			entry->resjunk = false;
			narrow_targetlist = lappend(narrow_targetlist, entry);
		}
	}
	if (narrow_targetlist == NIL ||
		list_length(narrow_targetlist) >= list_length(original_targetlist))
		return false;
	if (bloompg_log_transfer_steps)
	{
		StringInfoData columns;
		ListCell *target_cell;

		initStringInfo(&columns);
		foreach(target_cell, narrow_targetlist)
		{
			TargetEntry *entry = lfirst_node(TargetEntry, target_cell);
			Node *expression = strip_implicit_coercions((Node *) entry->expr);

			if (columns.len > 0)
				appendStringInfoChar(&columns, ',');
			if (IsA(expression, Var))
				appendStringInfo(&columns, "%u.%d",
					castNode(Var, expression)->varno,
					castNode(Var, expression)->varattno);
			else
				appendStringInfo(&columns, "expr:%d", (int) nodeTag(expression));
		}
		elog(LOG, "bloompg projected RTI %u from %d to %d columns [%s]",
			table->rti, list_length(original_targetlist),
			list_length(narrow_targetlist), columns.data);
		pfree(columns.data);
	}
	plan->targetlist = narrow_targetlist;
	*projected_columns = list_length(narrow_targetlist);
	return true;
}

void
bloompg_profile_append_projection_attributes(StringInfo json,
	BloomPGRelationState *state)
{
	List	   *targetlist = state->fallback_plan != NULL ?
		state->fallback_plan->targetlist : NIL;
	ListCell *cell;
	bool first = true;

	foreach(cell, targetlist)
	{
		TargetEntry *entry = lfirst_node(TargetEntry, cell);
		Node *expression = strip_implicit_coercions((Node *) entry->expr);

		if (!first)
			appendStringInfoChar(json, ',');
		first = false;
		appendStringInfo(json, "{\"resno\":%d,\"type_oid\":%u",
			entry->resno, exprType(expression));
		if (IsA(expression, Var))
			appendStringInfo(json, ",\"rti\":%u,\"attno\":%d",
				castNode(Var, expression)->varno,
				castNode(Var, expression)->varattno);
		else
			appendStringInfo(json, ",\"node_tag\":%d", (int) nodeTag(expression));
		appendStringInfoChar(json, '}');
	}
}

Node *
bloompg_normalize_syntax_vars(Node *node, void *context)
{
	(void) context;
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var *source = castNode(Var, node);
		Var *result = copyObject(source);

		if (result->varno < 0 && result->varnosyn > 0)
		{
			result->varno = result->varnosyn;
			result->varattno = result->varattnosyn;
		}
		return (Node *) result;
	}
	return expression_tree_mutator(node, bloompg_normalize_syntax_vars, NULL);
}

AttrNumber
bloompg_find_state_output_attribute(BloomPGRelationState *state,
									BloomPGColumnRef column)
{
	if (state->table->rte_kind != RTE_SUBQUERY)
		return bloompg_find_output_attribute(state->plan, column);
	/* SubqueryScan target lists retain the outer syntactic RTI/attribute in
	 * varnosyn/varattnosyn even when the physical output has been reordered by
	 * the parent reltarget. */
	{
		AttrNumber mapped = bloompg_find_output_attribute(state->plan, column);

		if (AttributeNumberIsValid(mapped))
			return mapped;
	}
	if (state->simple_subquery)
	{
		ListCell *cell;

		foreach(cell, state->table->subquery->targetList)
		{
			TargetEntry *entry = lfirst_node(TargetEntry, cell);
			Node *expression;
			Var *var;
			BloomPGColumnRef inner;

			if (entry->resjunk || entry->resno != column.attribute)
				continue;
			expression = strip_implicit_coercions((Node *) entry->expr);
			if (!IsA(expression, Var))
				return InvalidAttrNumber;
			var = castNode(Var, expression);
			if (var->varlevelsup != 0 || var->varno <= 0 ||
				var->varno > state->local_rtable_count ||
				state->local_to_global[var->varno] == 0)
				return InvalidAttrNumber;
			inner.rti = state->local_to_global[var->varno];
			inner.attribute = var->varattno;
			inner.type_oid = var->vartype;
			inner.collation = var->varcollid;
			return bloompg_find_output_attribute(state->plan, inner);
		}
		return InvalidAttrNumber;
	}

	/* A computed subquery output needs an ExprState, not a raw slot attribute.
	 * Positional guessing is unsafe because PG freely reorders reltargets. */
	return InvalidAttrNumber;
}

List *
bloompg_subquery_replacement_targetlist(BloomPGRelationState *state)
{
	List *source = bloompg_semantic_targetlist(state->plan);
	List *result = NIL;
	ListCell *cell;

	foreach(cell, source)
	{
		TargetEntry *source_entry = lfirst_node(TargetEntry, cell);
		TargetEntry *entry = copyObject(source_entry);
		Oid type_oid = exprType((Node *) source_entry->expr);
		int32 typmod = exprTypmod((Node *) source_entry->expr);
		Oid collation = exprCollation((Node *) source_entry->expr);

		if (source_entry->resjunk)
			entry->expr = (Expr *) makeNullConst(type_oid, typmod, collation);
		else
		{
			Node *expression = strip_implicit_coercions(
				(Node *) source_entry->expr);
			AttrNumber outer_attribute = InvalidAttrNumber;

			if (IsA(expression, Var))
			{
				Var *var = castNode(Var, expression);

				if ((Index) var->varnosyn == state->rti &&
					var->varattnosyn > 0)
					outer_attribute = var->varattnosyn;
				else if ((Index) var->varno == state->rti &&
					var->varattno > 0)
					outer_attribute = var->varattno;
			}
			if (!AttributeNumberIsValid(outer_attribute))
				return NIL;
			entry->expr = (Expr *) makeVar(state->rti, outer_attribute,
				type_oid, typmod, collation, 0);
		}
		result = lappend(result, entry);
	}
	return result;
}

static double
bloompg_plan_base_rows(PlannedStmt *candidate, Plan *plan)
{
	double rows = 0.0;
	List *children = NIL;
	ListCell *cell;
	Index rti;

	if (plan == NULL)
		return 0.0;
	if (bloompg_is_supported_scan(plan, &rti) &&
		rti > 0 && rti <= (Index) list_length(candidate->rtable))
	{
		RangeTblEntry *rte = list_nth_node(RangeTblEntry,
			candidate->rtable, rti - 1);

		if (rte->rtekind == RTE_RELATION)
		{
			HeapTuple tuple = SearchSysCache1(RELOID,
				ObjectIdGetDatum(rte->relid));

			if (HeapTupleIsValid(tuple))
			{
				Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);

				rows = Max(rows, (double) form->reltuples);
				ReleaseSysCache(tuple);
			}
		}
		else if (rte->rtekind == RTE_CTE && IsA(plan, CteScan))
		{
			int plan_id = castNode(CteScan, plan)->ctePlanId;

			if (plan_id > 0 && plan_id <= list_length(candidate->subplans))
				rows = Max(rows, bloompg_plan_base_rows(candidate,
					(Plan *) list_nth(candidate->subplans, plan_id - 1)));
		}
	}
	rows = Max(rows, bloompg_plan_base_rows(candidate, outerPlan(plan)));
	rows = Max(rows, bloompg_plan_base_rows(candidate, innerPlan(plan)));
	switch (nodeTag(plan))
	{
		case T_Append:
			children = castNode(Append, plan)->appendplans;
			break;
		case T_MergeAppend:
			children = castNode(MergeAppend, plan)->mergeplans;
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		case T_SubqueryScan:
			rows = Max(rows, bloompg_plan_base_rows(candidate,
				castNode(SubqueryScan, plan)->subplan));
			break;
		default:
			break;
	}
	foreach(cell, children)
		rows = Max(rows, bloompg_plan_base_rows(candidate,
			(Plan *) lfirst(cell)));
	return rows;
}

double
bloompg_relation_rows(PlannedStmt *candidate, Index rti, Plan *scan)
{
	RangeTblEntry *rte;
	HeapTuple tuple;
	double rows = scan->plan_rows;

	if (rti == 0 || rti > (Index) list_length(candidate->rtable))
		return Max(rows, 1.0);
	rte = list_nth_node(RangeTblEntry, candidate->rtable, rti - 1);
	if (rte->rtekind != RTE_RELATION)
		return Max(bloompg_plan_base_rows(candidate, scan), Max(rows, 1.0));
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(rte->relid));
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_class relation_form = (Form_pg_class) GETSTRUCT(tuple);

		if (relation_form->reltuples > 0)
			rows = relation_form->reltuples;
		ReleaseSysCache(tuple);
	}
	return Max(rows, 1.0);
}

void
bloompg_disable_parallel_flags(Plan *plan)
{
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return;
	plan->parallel_aware = false;
	plan->parallel_safe = false;
	bloompg_disable_parallel_flags(outerPlan(plan));
	bloompg_disable_parallel_flags(innerPlan(plan));
	switch (nodeTag(plan))
	{
		case T_Append:
			children = castNode(Append, plan)->appendplans;
			break;
		case T_MergeAppend:
			children = castNode(MergeAppend, plan)->mergeplans;
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		default:
			break;
	}
	foreach(cell, children)
		bloompg_disable_parallel_flags((Plan *) lfirst(cell));
}
