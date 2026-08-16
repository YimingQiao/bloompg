#include "postgres.h"

#include "bloompg/scan_quals.h"

#include "nodes/nodeFuncs.h"

/*
 * A P0 access path is only one physical implementation of a base-relation
 * scan.  Sampling and exact transfer need its semantic projection and local
 * predicates independently of whether PostgreSQL chose SeqScan, IndexScan,
 * BitmapHeapScan, or a TID path.  Keep that normalization in one place.
 */

static Node *
bloompg_index_only_var_mutator(Node *node, void *context)
{
	IndexOnlyScan *scan = context;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && castNode(Var, node)->varno == INDEX_VAR)
	{
		Var *var = castNode(Var, node);
		ListCell *cell;

		foreach(cell, scan->indextlist)
		{
			TargetEntry *entry = lfirst_node(TargetEntry, cell);

			if (entry->resno == var->varattno)
				return bloompg_index_only_var_mutator(
					(Node *) copyObject(entry->expr), context);
		}
		return NULL;
	}
	return expression_tree_mutator(node, bloompg_index_only_var_mutator,
		context);
}

bool
bloompg_scan_has_base_slot_semantics(const Plan *scan)
{
	return scan != NULL &&
		(IsA(scan, SeqScan) || IsA(scan, IndexScan) ||
		 IsA(scan, IndexOnlyScan) || IsA(scan, BitmapHeapScan) ||
		 IsA(scan, TidScan) || IsA(scan, TidRangeScan));
}

List *
bloompg_scan_base_quals(Plan *scan)
{
	List *quals;

	if (!bloompg_scan_has_base_slot_semantics(scan))
		return NIL;
	quals = copyObject(scan->qual);
	if (IsA(scan, IndexScan))
		quals = list_concat(quals,
			copyObject(castNode(IndexScan, scan)->indexqualorig));
	else if (IsA(scan, IndexOnlyScan))
	{
		IndexOnlyScan *index_scan = castNode(IndexOnlyScan, scan);

		quals = list_concat(quals, copyObject(index_scan->recheckqual));
		quals = (List *) bloompg_index_only_var_mutator((Node *) quals,
			index_scan);
	}
	else if (IsA(scan, BitmapHeapScan))
		quals = list_concat(quals,
			copyObject(castNode(BitmapHeapScan, scan)->bitmapqualorig));
	else if (IsA(scan, TidScan))
		quals = list_concat(quals,
			copyObject(castNode(TidScan, scan)->tidquals));
	else if (IsA(scan, TidRangeScan))
		quals = list_concat(quals,
			copyObject(castNode(TidRangeScan, scan)->tidrangequals));
	return quals;
}

SeqScan *
bloompg_make_seqscan_evaluator(Plan *source)
{
	SeqScan *scan;
	Plan *plan;
	List *targetlist;

	if (!bloompg_scan_has_base_slot_semantics(source))
		return NULL;
	targetlist = copyObject(source->targetlist);
	if (IsA(source, IndexOnlyScan))
		targetlist = (List *) bloompg_index_only_var_mutator(
			(Node *) targetlist, source);
	if (targetlist == NIL && source->targetlist != NIL)
		return NULL;

	scan = makeNode(SeqScan);
	plan = &scan->scan.plan;
	plan->startup_cost = source->startup_cost;
	plan->total_cost = source->total_cost;
	plan->plan_rows = source->plan_rows;
	plan->plan_width = source->plan_width;
	plan->parallel_aware = false;
	plan->parallel_safe = false;
	plan->async_capable = false;
	plan->plan_node_id = source->plan_node_id;
	plan->targetlist = targetlist;
	plan->qual = bloompg_scan_base_quals(source);
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->initPlan = copyObject(source->initPlan);
	plan->extParam = bms_copy(source->extParam);
	plan->allParam = bms_copy(source->allParam);
	scan->scan.scanrelid = ((Scan *) source)->scanrelid;
	return scan;
}
