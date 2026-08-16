#ifndef BLOOMPG_PLANNER_H
#define BLOOMPG_PLANNER_H

#include "postgres.h"

#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

/*
 * Plan a BloomPG-owned auxiliary query through the normal PostgreSQL planner
 * chain while the outer Bloom planner recursion guard is active.  Transfer
 * uses this entry point for a single-relation access query; callers must pass
 * a private Query because standard_planner() scribbles on it.
 */
extern PlannedStmt *bloompg_plan_auxiliary_query(Query *query,
	const char *query_string, int cursor_options, ParamListInfo bound_params);

#endif
