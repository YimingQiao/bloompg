#include "postgres.h"

#include "bloompg/scan.h"
#include "bloompg/runtime.h"
#include "bloompg/transfer.h"
#include "bloompg/sampling.h"

#include "nodes/plannodes.h"

/*
 * Release speculative transfer artifacts that the second PostgreSQL planner
 * pass did not keep.  This lives outside transfer.c deliberately: planning
 * decides which replacement wins, while this module only traces the final
 * plan and owns the corresponding runtime-lifetime cleanup.
 */

static bool
bloompg_plan_uses_materialization(Plan *plan, uint64 wanted_token)
{
	uint64 token;
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return false;
	if ((bloompg_memory_scan_info(plan, &token, NULL) ||
		 bloompg_empty_scan_info(plan, &token) ||
		 bloompg_tid_scan_info(plan, &token, NULL)) && token == wanted_token)
		return true;
	if (bloompg_plan_uses_materialization(outerPlan(plan), wanted_token) ||
		bloompg_plan_uses_materialization(innerPlan(plan), wanted_token))
		return true;
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
			return bloompg_plan_uses_materialization(
				castNode(SubqueryScan, plan)->subplan, wanted_token);
		default:
			break;
	}
	foreach(cell, children)
	{
		if (bloompg_plan_uses_materialization((Plan *) lfirst(cell),
				wanted_token))
			return true;
	}
	return false;
}

static bool
bloompg_plan_uses_filter(Plan *plan, uint64 wanted_token)
{
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return false;
	if (bloompg_filter_scan_uses_token(plan, wanted_token))
		return true;
	if (bloompg_plan_uses_filter(outerPlan(plan), wanted_token) ||
		bloompg_plan_uses_filter(innerPlan(plan), wanted_token))
		return true;
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
			return bloompg_plan_uses_filter(
				castNode(SubqueryScan, plan)->subplan, wanted_token);
		default:
			break;
	}
	foreach(cell, children)
	{
		if (bloompg_plan_uses_filter((Plan *) lfirst(cell), wanted_token))
			return true;
	}
	return false;
}

static bool
bloompg_statement_uses_materialization(PlannedStmt *statement, uint64 token)
{
	ListCell *cell;

	if (statement == NULL)
		return false;
	if (bloompg_plan_uses_materialization(statement->planTree, token))
		return true;
	foreach(cell, statement->subplans)
	{
		if (bloompg_plan_uses_materialization((Plan *) lfirst(cell), token))
			return true;
	}
	return false;
}

static bool
bloompg_statement_uses_filter(PlannedStmt *statement, uint64 token)
{
	ListCell *cell;

	if (statement == NULL)
		return false;
	if (bloompg_plan_uses_filter(statement->planTree, token))
		return true;
	foreach(cell, statement->subplans)
	{
		if (bloompg_plan_uses_filter((Plan *) lfirst(cell), token))
			return true;
	}
	return false;
}

void
bloompg_finish_replacements(List *replacements, PlannedStmt *final_plan)
{
	ListCell *cell;

	foreach(cell, replacements)
	{
		BloomPGReplacement *replacement = lfirst(cell);
		ListCell *filter_cell;

		if (replacement->kind == BLOOMPG_REPLACEMENT_FILTER)
		{
			foreach(filter_cell, replacement->filters)
			{
				BloomPGSampleFilter *filter = lfirst(filter_cell);

				if (!bloompg_statement_uses_filter(final_plan, filter->token))
					bloompg_runtime_release_filter(filter->token);
			}
			continue;
		}

		if (!bloompg_statement_uses_materialization(final_plan,
				replacement->token))
			bloompg_runtime_release_materialization(replacement->token);
		if (replacement->kind == BLOOMPG_REPLACEMENT_TID &&
			!bloompg_statement_uses_materialization(final_plan,
				replacement->fallback_token))
			bloompg_runtime_release_materialization(
				replacement->fallback_token);
	}
}
