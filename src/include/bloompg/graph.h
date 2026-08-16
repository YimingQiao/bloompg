#ifndef BLOOMPG_GRAPH_H
#define BLOOMPG_GRAPH_H

#include "postgres.h"

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/bitmapset.h"

typedef struct BloomPGColumnRef
{
	Index		rti;
	AttrNumber	attribute;
	Oid			type_oid;
	Oid			collation;
} BloomPGColumnRef;

typedef struct BloomPGJoinEdge
{
	BloomPGColumnRef left;
	BloomPGColumnRef right;
	Oid			operator_oid;
	Oid			input_collation;
	JoinType	join_type;
	bool		protect_left;
	bool		protect_right;
} BloomPGJoinEdge;

typedef struct BloomPGTable
{
	Index		rti;
	Oid			relation_oid;
	RTEKind		rte_kind;
	char	   *alias;
	Query	   *subquery;
	Bitmapset  *required_attributes;
} BloomPGTable;

typedef struct BloomPGJoinGraph
{
	MemoryContext memory_context;
	int			rtable_count;
	List	   *tables;
	List	   *edges;
	bool		has_outer_join;
	bool		has_set_operation;
	bool		has_recursive_scope;
} BloomPGJoinGraph;

extern BloomPGJoinGraph *bloompg_graph_build(Query *query,
											 MemoryContext context);
extern void bloompg_graph_augment_plan(BloomPGJoinGraph *graph,
										  PlannedStmt *statement);
extern char *bloompg_graph_trace(const BloomPGJoinGraph *graph);

#endif
