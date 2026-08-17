#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/scan.h"
#include "bloompg/scan_internal.h"
#include "bloompg/filter.h"
#include "bloompg/runtime.h"

#include "access/tableam.h"
#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "port/atomics.h"
#include "utils/snapmgr.h"

#include <math.h>

static List *
bloompg_oid_array_to_list(const Oid *values, int count)
{
	List *result = NIL;
	int i;

	for (i = 0; i < count; i++)
		result = lappend(result, makeString(psprintf("%u", values[i])));
	return result;
}

static List *
bloompg_attr_array_to_list(const AttrNumber *values, int count)
{
	List *result = NIL;
	int i;

	for (i = 0; i < count; i++)
		result = lappend_int(result, values[i]);
	return result;
}

Oid *
bloompg_list_to_oid_array(List *values, int expected)
{
	Oid *result = palloc_array(Oid, expected);
	ListCell *cell;
	int index = 0;

	foreach(cell, values)
	{
		if (index >= expected)
			elog(ERROR, "invalid bloompg custom scan OID metadata");
		result[index++] = (Oid) strtoul(strVal(lfirst(cell)), NULL, 10);
	}
	if (index != expected)
		elog(ERROR, "incomplete bloompg custom scan OID metadata");
	return result;
}

AttrNumber *
bloompg_list_to_attr_array(List *values, int expected)
{
	AttrNumber *result = palloc_array(AttrNumber, expected);
	ListCell *cell;
	int index = 0;

	foreach(cell, values)
	{
		if (index >= expected)
			elog(ERROR, "invalid bloompg custom scan attribute metadata");
		result[index++] = (AttrNumber) lfirst_int(cell);
	}
	if (index != expected)
		elog(ERROR, "incomplete bloompg custom scan attribute metadata");
	return result;
}

/*
 * The planner has already run set_plan_references() when BloomPG installs the
 * wrapper.  Build the INDEX_VAR target list that core would normally create
 * for a CustomScan so its raw scan tuple is positionally identical to the
 * wrapped plan's output tuple.
 */
static List *
bloompg_make_index_targetlist(List *scan_targetlist)
{
	List *result = NIL;
	ListCell *cell;

	foreach(cell, scan_targetlist)
	{
		TargetEntry *source = lfirst_node(TargetEntry, cell);
		TargetEntry *entry = copyObject(source);

		entry->expr = (Expr *) makeVarFromTargetEntry(INDEX_VAR, source);
		result = lappend(result, entry);
	}
	return result;
}

Plan *
bloompg_make_apply_scan(Plan *probe_plan,
						Plan *build_plan,
						const BloomPGApplySpec *spec,
						int plan_node_id)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan *plan = &scan->scan.plan;
	List *metadata = NIL;
	List *scan_targetlist = probe_plan->targetlist;

	if (IsA(probe_plan, CustomScan) &&
		castNode(CustomScan, probe_plan)->custom_scan_tlist != NIL)
		scan_targetlist = castNode(CustomScan, probe_plan)->custom_scan_tlist;

	plan->startup_cost = probe_plan->startup_cost;
	plan->total_cost = probe_plan->total_cost;
	plan->plan_rows = probe_plan->plan_rows;
	plan->plan_width = probe_plan->plan_width;
	plan->parallel_aware = false;
	plan->parallel_safe = false;
	plan->async_capable = false;
	plan->plan_node_id = plan_node_id;
	plan->targetlist = bloompg_make_index_targetlist(scan_targetlist);
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->initPlan = NIL;
	plan->extParam = bms_union(probe_plan->extParam, build_plan->extParam);
	plan->allParam = bms_union(probe_plan->allParam, build_plan->allParam);

	scan->scan.scanrelid = 0;
	scan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	scan->custom_plans = list_make2(probe_plan, build_plan);
	scan->custom_exprs = NIL;
	scan->custom_scan_tlist = copyObject(scan_targetlist);
	scan->custom_relids = NULL;
	scan->methods = &bloompg_apply_scan_methods;

	metadata = lappend(metadata, makeString(psprintf(UINT64_FORMAT, spec->token)));
	metadata = lappend(metadata, makeString(psprintf(INT64_FORMAT, spec->estimated_rows)));
	metadata = lappend(metadata, makeInteger(spec->nkeys));
	metadata = lappend(metadata, makeInteger(spec->build_is_left ? 1 : 0));
	metadata = lappend(metadata,
					   bloompg_oid_array_to_list(spec->operators, spec->nkeys));
	metadata = lappend(metadata,
					   bloompg_oid_array_to_list(spec->collations, spec->nkeys));
	metadata = lappend(metadata,
					   bloompg_attr_array_to_list(spec->build_attributes, spec->nkeys));
	metadata = lappend(metadata,
					   bloompg_attr_array_to_list(spec->probe_attributes, spec->nkeys));
	scan->custom_private = metadata;
	return plan;
}

Plan *
bloompg_make_memory_scan(Plan *fallback_plan,
						 uint64 token,
						 uint64 rows,
						 int plan_node_id)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan *plan = &scan->scan.plan;
	List *scan_targetlist = fallback_plan->targetlist;
	List *metadata = NIL;

	if (IsA(fallback_plan, CustomScan) &&
		castNode(CustomScan, fallback_plan)->custom_scan_tlist != NIL)
		scan_targetlist = castNode(CustomScan, fallback_plan)->custom_scan_tlist;

	plan->startup_cost = 0;
	plan->total_cost = (Cost) rows * cpu_tuple_cost;
	plan->plan_rows = (double) rows;
	plan->plan_width = fallback_plan->plan_width;
	plan->parallel_aware = false;
	plan->parallel_safe = false;
	plan->async_capable = false;
	plan->plan_node_id = plan_node_id;
	plan->targetlist = bloompg_make_index_targetlist(scan_targetlist);
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->initPlan = NIL;
	plan->extParam = bms_copy(fallback_plan->extParam);
	plan->allParam = bms_copy(fallback_plan->allParam);

	scan->scan.scanrelid = 0;
	scan->flags = CUSTOMPATH_SUPPORT_PROJECTION |
		CUSTOMPATH_SUPPORT_BACKWARD_SCAN;
	scan->custom_plans = list_make1(fallback_plan);
	scan->custom_exprs = NIL;
	scan->custom_scan_tlist = copyObject(scan_targetlist);
	scan->custom_relids = NULL;
	scan->methods = &bloompg_memory_scan_methods;
	metadata = lappend(metadata, makeString(psprintf(UINT64_FORMAT, token)));
	metadata = lappend(metadata, makeString(psprintf(UINT64_FORMAT, rows)));
	scan->custom_private = metadata;
	return plan;
}

/* Planner-stage variant.  Core will assign costs, plan-node IDs, parameter
 * sets, and convert the target list to INDEX_VAR in set_plan_references(). */
Plan *
bloompg_make_memory_scan_path(List *targetlist,
						  List *scan_targetlist,
						  Plan *fallback_plan,
						  uint64 token,
						  uint64 rows)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan *plan = &scan->scan.plan;
	List *metadata = NIL;

	plan->targetlist = copyObject(targetlist);
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	scan->scan.scanrelid = 0;
	scan->flags = CUSTOMPATH_SUPPORT_PROJECTION |
		CUSTOMPATH_SUPPORT_BACKWARD_SCAN;
	scan->custom_plans = list_make1(fallback_plan);
	scan->custom_exprs = NIL;
	scan->custom_scan_tlist = copyObject(scan_targetlist);
	scan->custom_relids = NULL;
	scan->methods = &bloompg_memory_scan_methods;
	metadata = lappend(metadata, makeString(psprintf(UINT64_FORMAT, token)));
	metadata = lappend(metadata, makeString(psprintf(UINT64_FORMAT, rows)));
	scan->custom_private = metadata;
	return plan;
}

Plan *
bloompg_make_empty_scan_path(List *targetlist,
						 List *scan_targetlist,
						 Plan *fallback_plan,
						 uint64 token)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan *plan = &scan->scan.plan;
	List *metadata = NIL;

	plan->targetlist = copyObject(targetlist);
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	scan->scan.scanrelid = 0;
	scan->flags = CUSTOMPATH_SUPPORT_PROJECTION |
		CUSTOMPATH_SUPPORT_BACKWARD_SCAN;
	scan->custom_plans = list_make1(fallback_plan);
	scan->custom_exprs = NIL;
	scan->custom_scan_tlist = copyObject(scan_targetlist);
	scan->custom_relids = NULL;
	scan->methods = &bloompg_empty_scan_methods;
	metadata = lappend(metadata, makeString(psprintf(UINT64_FORMAT, token)));
	metadata = lappend(metadata, makeString(pstrdup("0")));
	scan->custom_private = metadata;
	return plan;
}

Plan *
bloompg_make_tid_scan_path(List *targetlist,
						   Plan *fallback_plan,
						   Index rti,
						   uint64 token,
						   uint64 rows)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan *plan = &scan->scan.plan;
	List *metadata = NIL;

	plan->targetlist = copyObject(targetlist);
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	scan->scan.scanrelid = rti;
	scan->flags = CUSTOMPATH_SUPPORT_PROJECTION |
		CUSTOMPATH_SUPPORT_BACKWARD_SCAN;
	scan->custom_plans = list_make1(fallback_plan);
	scan->custom_exprs = NIL;
	scan->custom_scan_tlist = NIL;
	scan->custom_relids = bms_make_singleton(rti);
	scan->methods = &bloompg_tid_scan_methods;
	metadata = lappend(metadata, makeString(psprintf(UINT64_FORMAT, token)));
	metadata = lappend(metadata, makeString(psprintf(UINT64_FORMAT, rows)));
	scan->custom_private = metadata;
	return plan;
}

Plan *
bloompg_make_filter_scan_path(List *targetlist,
							  List *scan_targetlist,
							  Plan *fallback_plan,
							  List *filter_metadata)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan *plan = &scan->scan.plan;

	plan->targetlist = copyObject(targetlist);
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	scan->scan.scanrelid = 0;
	scan->flags = CUSTOMPATH_SUPPORT_PROJECTION |
		CUSTOMPATH_SUPPORT_BACKWARD_SCAN;
	scan->custom_plans = list_make1(fallback_plan);
	scan->custom_exprs = NIL;
	scan->custom_scan_tlist = copyObject(scan_targetlist);
	scan->custom_relids = NULL;
	scan->methods = &bloompg_filter_scan_methods;
	scan->custom_private = copyObject(filter_metadata);
	return plan;
}

/*
 * Post-planner variant used by optimizer-time transfer execution.  The
 * fallback plan has already passed set_plan_references(), so expose its
 * output through INDEX_VAR exactly like the other executable CustomScan
 * constructors in this file.  Marking this node parallel-aware lets core
 * serialize the registered filters into Gather DSM before workers start.
 */
Plan *
bloompg_make_filter_scan(Plan *fallback_plan,
						 List *filter_metadata,
						 int plan_node_id,
						 bool parallel_aware,
						 bool prefilter)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan *plan = &scan->scan.plan;
	List *scan_targetlist = fallback_plan->targetlist;

	if (IsA(fallback_plan, CustomScan) &&
		castNode(CustomScan, fallback_plan)->custom_scan_tlist != NIL)
		scan_targetlist = castNode(CustomScan,
			fallback_plan)->custom_scan_tlist;

	plan->startup_cost = fallback_plan->startup_cost;
	plan->total_cost = fallback_plan->total_cost;
	plan->plan_rows = fallback_plan->plan_rows;
	plan->plan_width = fallback_plan->plan_width;
	plan->parallel_aware = parallel_aware;
	plan->parallel_safe = fallback_plan->parallel_safe;
	plan->async_capable = false;
	plan->plan_node_id = plan_node_id;
	plan->targetlist = bloompg_make_index_targetlist(scan_targetlist);
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->initPlan = NIL;
	plan->extParam = bms_copy(fallback_plan->extParam);
	plan->allParam = bms_copy(fallback_plan->allParam);

	scan->scan.scanrelid = 0;
	scan->flags = CUSTOMPATH_SUPPORT_PROJECTION |
		CUSTOMPATH_SUPPORT_BACKWARD_SCAN;
	scan->custom_plans = list_make1(fallback_plan);
	scan->custom_exprs = NIL;
	scan->custom_scan_tlist = copyObject(scan_targetlist);
	scan->custom_relids = NULL;
	scan->methods = prefilter ? &bloompg_prefilter_scan_methods :
		&bloompg_filter_scan_methods;
	scan->custom_private = copyObject(filter_metadata);
	return plan;
}

/* Post-planner worker-side column packet.  The raw CustomScan exposes one
 * bytea, while its child retains the relation's already-pruned projection. */
Plan *
bloompg_make_pack_scan(Plan *fallback_plan, int plan_node_id,
	int parallel_workers)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan *plan = &scan->scan.plan;
	TargetEntry *raw_entry = makeTargetEntry(
		(Expr *) makeNullConst(BYTEAOID, -1, InvalidOid), 1,
		pstrdup("__bloompg_vector_packet"), false);
	List *raw_targetlist = list_make1(raw_entry);

	plan->startup_cost = fallback_plan->startup_cost;
	plan->total_cost = fallback_plan->total_cost;
	plan->plan_rows = ceil(Max(fallback_plan->plan_rows, 1.0) /
		(double) BLOOMPG_VECTOR_SIZE);
	plan->plan_width = fallback_plan->plan_width >
		(int) (MaxAllocSize / BLOOMPG_VECTOR_SIZE) ? (int) MaxAllocSize :
		Max(fallback_plan->plan_width, 1) * BLOOMPG_VECTOR_SIZE;
	plan->parallel_aware = false;
	plan->parallel_safe = fallback_plan->parallel_safe;
	plan->async_capable = false;
	plan->plan_node_id = plan_node_id;
	plan->targetlist = bloompg_make_index_targetlist(raw_targetlist);
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->initPlan = NIL;
	plan->extParam = bms_copy(fallback_plan->extParam);
	plan->allParam = bms_copy(fallback_plan->allParam);

	scan->scan.scanrelid = 0;
	scan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	scan->custom_plans = list_make1(fallback_plan);
	scan->custom_exprs = NIL;
	scan->custom_scan_tlist = raw_targetlist;
	scan->custom_relids = NULL;
	scan->custom_private = list_make1(makeInteger(parallel_workers));
	scan->methods = &bloompg_pack_scan_methods;
	return plan;
}

bool
bloompg_memory_scan_info(Plan *plan, uint64 *token, uint64 *rows)
{
	CustomScan *scan;

	if (plan == NULL || !IsA(plan, CustomScan))
		return false;
	scan = castNode(CustomScan, plan);
	if (scan->methods != &bloompg_memory_scan_methods ||
		list_length(scan->custom_private) != 2)
		return false;
	if (token != NULL)
		*token = strtoull(strVal(list_nth(scan->custom_private, 0)), NULL, 10);
	if (rows != NULL)
		*rows = strtoull(strVal(list_nth(scan->custom_private, 1)), NULL, 10);
	return true;
}

bool
bloompg_empty_scan_info(Plan *plan, uint64 *token)
{
	CustomScan *scan;

	if (plan == NULL || !IsA(plan, CustomScan))
		return false;
	scan = castNode(CustomScan, plan);
	if (scan->methods != &bloompg_empty_scan_methods ||
		list_length(scan->custom_private) != 2)
		return false;
	if (token != NULL)
		*token = strtoull(strVal(list_nth(scan->custom_private, 0)), NULL, 10);
	return true;
}

bool
bloompg_tid_scan_info(Plan *plan, uint64 *token, uint64 *rows)
{
	CustomScan *scan;

	if (plan == NULL || !IsA(plan, CustomScan))
		return false;
	scan = castNode(CustomScan, plan);
	if (scan->methods != &bloompg_tid_scan_methods ||
		list_length(scan->custom_private) != 2)
		return false;
	if (token != NULL)
		*token = strtoull(strVal(list_nth(scan->custom_private, 0)), NULL, 10);
	if (rows != NULL)
		*rows = strtoull(strVal(list_nth(scan->custom_private, 1)), NULL, 10);
	return true;
}

bool
bloompg_filter_scan_uses_token(Plan *plan, uint64 token)
{
	CustomScan *scan;
	ListCell *cell;

	if (plan == NULL || !IsA(plan, CustomScan))
		return false;
	scan = castNode(CustomScan, plan);
	if (scan->methods != &bloompg_filter_scan_methods &&
		scan->methods != &bloompg_prefilter_scan_methods)
		return false;
	foreach(cell, scan->custom_private)
	{
		List *metadata = lfirst_node(List, cell);

		if (list_length(metadata) == 3 &&
			strtoull(strVal(linitial(metadata)), NULL, 10) == token)
			return true;
	}
	return false;
}

void
bloompg_scan_init(void)
{
	RegisterCustomScanMethods(&bloompg_apply_scan_methods);
	RegisterCustomScanMethods(&bloompg_memory_scan_methods);
	RegisterCustomScanMethods(&bloompg_empty_scan_methods);
	RegisterCustomScanMethods(&bloompg_tid_scan_methods);
	RegisterCustomScanMethods(&bloompg_filter_scan_methods);
	RegisterCustomScanMethods(&bloompg_prefilter_scan_methods);
	RegisterCustomScanMethods(&bloompg_pack_scan_methods);
}
