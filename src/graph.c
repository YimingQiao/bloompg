#include "postgres.h"

#include "bloompg/bloompg.h"
#include "bloompg/graph.h"

#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

typedef struct BloomPGQualContext
{
	BloomPGJoinGraph *graph;
	JoinType join_type;
	Bitmapset *left_relids;
	Bitmapset *right_relids;
} BloomPGQualContext;

typedef struct BloomPGRequiredAttributeContext
{
	Index		rti;
	Index		sublevels_up;
	Bitmapset *attributes;
	bool		unsafe_reference;
} BloomPGRequiredAttributeContext;

static Bitmapset *bloompg_jointree_relids(Node *node);
static void bloompg_visit_jointree(Node *node, BloomPGJoinGraph *graph);
static void bloompg_visit_plan(Plan *plan, BloomPGJoinGraph *graph);

static bool
bloompg_required_attribute_walker(Node *node, void *context)
{
	BloomPGRequiredAttributeContext *required = context;

	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var *variable = castNode(Var, node);

		if ((Index) variable->varno == required->rti &&
			variable->varlevelsup == required->sublevels_up)
		{
			if (!AttributeNumberIsValid(variable->varattno))
			{
				required->unsafe_reference = true;
				return true;
			}
			required->attributes = bms_add_member(required->attributes,
				variable->varattno - FirstLowInvalidHeapAttributeNumber);
		}
		return false;
	}
	if (IsA(node, Query))
	{
		bool aborted;

		required->sublevels_up++;
		aborted = query_tree_walker(castNode(Query, node),
			bloompg_required_attribute_walker, required,
			QTW_IGNORE_JOINALIASES);
		required->sublevels_up--;
		return aborted;
	}
	return expression_tree_walker(node, bloompg_required_attribute_walker,
		context);
}

static Bitmapset *
bloompg_query_required_attributes(Query *query, Index rti)
{
	BloomPGRequiredAttributeContext required;

	if (query == NULL)
		return NULL;
	MemSet(&required, 0, sizeof(required));
	required.rti = rti;
	query_tree_walker(query, bloompg_required_attribute_walker, &required,
		QTW_IGNORE_JOINALIASES);

	/* A whole-row or system-column reference needs the scan's original tuple
	 * shape.  Positive attributes, including correlated references from any
	 * nested SubLink level, are safe to project independently. */
	if (required.unsafe_reference)
		return NULL;
	return required.attributes;
}

static Bitmapset *
bloompg_find_pulled_required_attributes(Query *query,
	const RangeTblEntry *wanted)
{
	ListCell *cell;
	Index rti = 0;
	const char *wanted_alias = wanted->eref != NULL ?
		wanted->eref->aliasname : NULL;

	if (query == NULL)
		return NULL;
	foreach(cell, query->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, cell);
		const char *alias;
		Bitmapset *nested;

		rti++;
		alias = rte->eref != NULL ? rte->eref->aliasname : NULL;
		if (rte->rtekind == RTE_RELATION && wanted->rtekind == RTE_RELATION &&
			rte->relid == wanted->relid &&
			(wanted_alias == NULL || alias == NULL ||
			 strcmp(wanted_alias, alias) == 0))
			return bloompg_query_required_attributes(query, rti);
		if (rte->rtekind != RTE_SUBQUERY)
			continue;
		nested = bloompg_find_pulled_required_attributes(rte->subquery, wanted);
		if (nested != NULL)
			return nested;
	}
	return NULL;
}

static bool
bloompg_same_edge(const BloomPGJoinEdge *left, const BloomPGJoinEdge *right)
{
	return left->left.rti == right->left.rti &&
		left->left.attribute == right->left.attribute &&
		left->right.rti == right->right.rti &&
		left->right.attribute == right->right.attribute &&
		left->operator_oid == right->operator_oid &&
		left->input_collation == right->input_collation &&
		left->protect_left == right->protect_left &&
		left->protect_right == right->protect_right;
}

static void
bloompg_add_plan_edge(BloomPGJoinGraph *graph,
					  BloomPGColumnRef first,
					  BloomPGColumnRef second,
					  Oid operator_oid,
					  Oid input_collation,
					  JoinType join_type,
					  bool protect_first,
					  bool protect_second)
{
	BloomPGJoinEdge *edge;
	ListCell *cell;

	if (first.rti == second.rti)
		return;
	edge = MemoryContextAllocZero(graph->memory_context,
		sizeof(BloomPGJoinEdge));
	edge->left = first;
	edge->right = second;
	edge->operator_oid = operator_oid;
	edge->input_collation = input_collation;
	edge->join_type = join_type;
	edge->protect_left = protect_first;
	edge->protect_right = protect_second;
	if (edge->left.rti > edge->right.rti)
	{
		BloomPGColumnRef swap_column = edge->left;
		bool swap_protection = edge->protect_left;
		Oid commutator = get_commutator(edge->operator_oid);

		if (!OidIsValid(commutator))
		{
			pfree(edge);
			return;
		}
		edge->left = edge->right;
		edge->right = swap_column;
		edge->protect_left = edge->protect_right;
		edge->protect_right = swap_protection;
		edge->operator_oid = commutator;
	}
	foreach(cell, graph->edges)
	{
		BloomPGJoinEdge *existing = lfirst(cell);

		/* Query-tree edges carry the most conservative outer-join
		 * protections.  Do not add a second physical-plan copy merely because
		 * the core planner proved that join reducible for this plan. */
		if (bloompg_same_edge(existing, edge) ||
			(existing->left.rti == edge->left.rti &&
			 existing->left.attribute == edge->left.attribute &&
			 existing->right.rti == edge->right.rti &&
			 existing->right.attribute == edge->right.attribute &&
			 existing->operator_oid == edge->operator_oid &&
			 existing->input_collation == edge->input_collation))
		{
			pfree(edge);
			return;
		}
	}
	graph->edges = lappend(graph->edges, edge);
}

static bool
bloompg_extract_var(Node *node, BloomPGColumnRef *column)
{
	Var *var;

	node = strip_implicit_coercions(node);
	if (node == NULL || !IsA(node, Var))
		return false;
	var = castNode(Var, node);
	if (var->varlevelsup != 0 || var->varno <= 0 || var->varattno <= 0)
		return false;

	column->rti = var->varno;
	column->attribute = var->varattno;
	column->type_oid = var->vartype;
	column->collation = var->varcollid;
	return true;
}

static void
bloompg_record_equality(Node *node, BloomPGQualContext *context)
{
	OpExpr *expression;
	BloomPGJoinEdge *edge;
	Node *left_argument;
	Node *right_argument;
	ListCell *cell;

	if (node == NULL)
		return;

	/* Only implicit conjunctions are safe to turn into a composite filter. */
	if (IsA(node, BoolExpr) && castNode(BoolExpr, node)->boolop == AND_EXPR)
	{
		foreach(cell, castNode(BoolExpr, node)->args)
			bloompg_record_equality(lfirst(cell), context);
		return;
	}
	if (!IsA(node, OpExpr))
		return;

	expression = castNode(OpExpr, node);
	if (bloompg_log_transfer_steps)
		elog(LOG, "bloompg graph saw operator %u with %d arguments (hashjoinable=%d)",
			 expression->opno, list_length(expression->args),
			 list_length(expression->args) == 2 ?
			 op_hashjoinable(expression->opno,
							 exprType(linitial(expression->args))) : 0);
	if (list_length(expression->args) != 2 ||
		!op_hashjoinable(expression->opno,
						 exprType(linitial(expression->args))) ||
		!op_strict(expression->opno))
		return;

	left_argument = linitial(expression->args);
	right_argument = lsecond(expression->args);
	edge = MemoryContextAllocZero(context->graph->memory_context,
								  sizeof(BloomPGJoinEdge));
	if (!bloompg_extract_var(left_argument, &edge->left) ||
		!bloompg_extract_var(right_argument, &edge->right) ||
		edge->left.rti == edge->right.rti)
	{
		pfree(edge);
		return;
	}
	if (edge->left.rti > edge->right.rti)
	{
		BloomPGColumnRef swap = edge->left;
		Oid commutator = get_commutator(expression->opno);

		if (!OidIsValid(commutator))
		{
			pfree(edge);
			return;
		}
		edge->left = edge->right;
		edge->right = swap;
		edge->operator_oid = commutator;
	}
	else
		edge->operator_oid = expression->opno;

	edge->input_collation = expression->inputcollid;
	edge->join_type = context->join_type;
	switch (context->join_type)
	{
		case JOIN_LEFT:
			edge->protect_left =
				bms_is_member(edge->left.rti, context->left_relids);
			edge->protect_right =
				bms_is_member(edge->right.rti, context->left_relids);
			break;
		case JOIN_RIGHT:
			edge->protect_left =
				bms_is_member(edge->left.rti, context->right_relids);
			edge->protect_right =
				bms_is_member(edge->right.rti, context->right_relids);
			break;
		case JOIN_FULL:
			edge->protect_left = true;
			edge->protect_right = true;
			break;
		case JOIN_ANTI:
			/* The preserved anti-join side cannot use a positive membership BF. */
			edge->protect_left =
				bms_is_member(edge->left.rti, context->left_relids);
			edge->protect_right =
				bms_is_member(edge->right.rti, context->left_relids);
			break;
		case JOIN_RIGHT_ANTI:
			edge->protect_left =
				bms_is_member(edge->left.rti, context->right_relids);
			edge->protect_right =
				bms_is_member(edge->right.rti, context->right_relids);
			break;
		default:
			break;
	}
	foreach(cell, context->graph->edges)
	{
		if (bloompg_same_edge(lfirst(cell), edge))
		{
			pfree(edge);
			return;
		}
	}
	context->graph->edges = lappend(context->graph->edges, edge);
}

static Bitmapset *
bloompg_jointree_relids(Node *node)
{
	Bitmapset *result = NULL;

	if (node == NULL)
		return NULL;
	if (IsA(node, RangeTblRef))
		return bms_make_singleton(castNode(RangeTblRef, node)->rtindex);
	if (IsA(node, JoinExpr))
	{
		JoinExpr *join = castNode(JoinExpr, node);

		result = bms_union(bloompg_jointree_relids(join->larg),
						   bloompg_jointree_relids(join->rarg));
		return result;
	}
	if (IsA(node, FromExpr))
	{
		FromExpr *from = castNode(FromExpr, node);
		ListCell *cell;

		foreach(cell, from->fromlist)
			result = bms_join(result,
							  bloompg_jointree_relids(lfirst(cell)));
		return result;
	}
	return NULL;
}

static void
bloompg_visit_jointree(Node *node, BloomPGJoinGraph *graph)
{
	if (node == NULL)
		return;
	if (IsA(node, JoinExpr))
	{
		JoinExpr *join = castNode(JoinExpr, node);
		BloomPGQualContext context;

		if (join->jointype != JOIN_INNER)
			graph->has_outer_join = true;
		context.graph = graph;
		context.join_type = join->jointype;
		context.left_relids = bloompg_jointree_relids(join->larg);
		context.right_relids = bloompg_jointree_relids(join->rarg);
		bloompg_record_equality(join->quals, &context);
		bloompg_visit_jointree(join->larg, graph);
		bloompg_visit_jointree(join->rarg, graph);
		bms_free(context.left_relids);
		bms_free(context.right_relids);
		return;
	}
	if (IsA(node, FromExpr))
	{
		FromExpr *from = castNode(FromExpr, node);
		BloomPGQualContext context;
		ListCell *cell;

		context.graph = graph;
		context.join_type = JOIN_INNER;
		context.left_relids = NULL;
		context.right_relids = NULL;
		bloompg_record_equality(from->quals, &context);
		foreach(cell, from->fromlist)
			bloompg_visit_jointree(lfirst(cell), graph);
	}
}

static TargetEntry *
bloompg_plan_target_entry(Plan *plan, AttrNumber attribute)
{
	ListCell *cell;

	if (plan == NULL || attribute <= 0)
		return NULL;
	foreach(cell, plan->targetlist)
	{
		TargetEntry *entry = lfirst_node(TargetEntry, cell);

		if (entry->resno == attribute)
			return entry;
	}
	return NULL;
}

static bool
bloompg_resolve_plan_column(Plan *context, Node *node,
							BloomPGColumnRef *column, int depth)
{
	Var *var;
	Plan *child;
	TargetEntry *entry;

	if (context == NULL || node == NULL || depth > 64)
		return false;
	node = strip_implicit_coercions(node);
	if (!IsA(node, Var))
		return false;
	var = castNode(Var, node);
	if (var->varlevelsup != 0 || var->varattno <= 0)
		return false;
	if (var->varno == OUTER_VAR)
		child = outerPlan(context);
	else if (var->varno == INNER_VAR)
		child = innerPlan(context);
	else if (var->varno == INDEX_VAR)
	{
		/* Index-only target lists refer to physical index columns with
		 * INDEX_VAR, while varnosyn/varattnosyn retain the base-table
		 * identity needed by the transfer graph. */
		if (var->varnosyn <= 0 || var->varattnosyn <= 0)
			return false;
		column->rti = var->varnosyn;
		column->attribute = var->varattnosyn;
		column->type_oid = var->vartype;
		column->collation = var->varcollid;
		return true;
	}
	else if (var->varno > 0)
	{
		column->rti = var->varno;
		column->attribute = var->varattno;
		column->type_oid = var->vartype;
		column->collation = var->varcollid;
		return true;
	}
	else
		return false;
	entry = bloompg_plan_target_entry(child, var->varattno);
	if (entry == NULL)
		return false;
	return bloompg_resolve_plan_column(child, (Node *) entry->expr,
		column, depth + 1);
}

static Bitmapset *
bloompg_plan_scan_relids(Plan *plan)
{
	Bitmapset *result = NULL;
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return NULL;
	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
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
			if (((Scan *) plan)->scanrelid > 0)
				result = bms_add_member(result,
					((Scan *) plan)->scanrelid);
			break;
		default:
			break;
	}
	result = bms_join(result, bloompg_plan_scan_relids(outerPlan(plan)));
	result = bms_join(result, bloompg_plan_scan_relids(innerPlan(plan)));
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
		case T_SubqueryScan:
			result = bms_join(result, bloompg_plan_scan_relids(
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
			bloompg_plan_scan_relids((Plan *) lfirst(cell)));
	return result;
}

static void
bloompg_record_plan_equalities(Plan *join_plan, List *clauses,
							   JoinType join_type,
							   BloomPGJoinGraph *graph)
{
	Bitmapset *outer_relids = bloompg_plan_scan_relids(outerPlan(join_plan));
	Bitmapset *inner_relids = bloompg_plan_scan_relids(innerPlan(join_plan));
	ListCell *cell;

	foreach(cell, clauses)
	{
		Node *node = lfirst(cell);
		OpExpr *expression;
		BloomPGColumnRef first;
		BloomPGColumnRef second;
		bool protect_first = false;
		bool protect_second = false;

		if (!IsA(node, OpExpr))
			continue;
		expression = castNode(OpExpr, node);
		if (list_length(expression->args) != 2 ||
			!op_hashjoinable(expression->opno,
				exprType(linitial(expression->args))) ||
			!op_strict(expression->opno) ||
			!bloompg_resolve_plan_column(join_plan,
				(Node *) linitial(expression->args), &first, 0) ||
			!bloompg_resolve_plan_column(join_plan,
				(Node *) lsecond(expression->args), &second, 0))
			continue;

		switch (join_type)
		{
			case JOIN_LEFT:
				protect_first = bms_is_member(first.rti, outer_relids);
				protect_second = bms_is_member(second.rti, outer_relids);
				break;
			case JOIN_RIGHT:
				protect_first = bms_is_member(first.rti, inner_relids);
				protect_second = bms_is_member(second.rti, inner_relids);
				break;
			case JOIN_FULL:
				protect_first = true;
				protect_second = true;
				break;
			case JOIN_ANTI:
				protect_first = bms_is_member(first.rti, outer_relids);
				protect_second = bms_is_member(second.rti, outer_relids);
				break;
			case JOIN_RIGHT_ANTI:
				protect_first = bms_is_member(first.rti, inner_relids);
				protect_second = bms_is_member(second.rti, inner_relids);
				break;
			default:
				break;
		}
		bloompg_add_plan_edge(graph, first, second, expression->opno,
			expression->inputcollid, join_type,
			protect_first, protect_second);
	}
	bms_free(outer_relids);
	bms_free(inner_relids);
}

static void
bloompg_visit_plan(Plan *plan, BloomPGJoinGraph *graph)
{
	List *children = NIL;
	ListCell *cell;

	if (plan == NULL)
		return;
	if (IsA(plan, HashJoin))
	{
		HashJoin *join = castNode(HashJoin, plan);

		bloompg_record_plan_equalities(plan, join->hashclauses,
			join->join.jointype, graph);
	}
	else if (IsA(plan, MergeJoin))
	{
		MergeJoin *join = castNode(MergeJoin, plan);

		bloompg_record_plan_equalities(plan, join->mergeclauses,
			join->join.jointype, graph);
	}
	else if (IsA(plan, NestLoop))
	{
		NestLoop *join = castNode(NestLoop, plan);

		bloompg_record_plan_equalities(plan, join->join.joinqual,
			join->join.jointype, graph);
	}
	bloompg_visit_plan(outerPlan(plan), graph);
	bloompg_visit_plan(innerPlan(plan), graph);
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
		case T_SubqueryScan:
			bloompg_visit_plan(castNode(SubqueryScan, plan)->subplan, graph);
			break;
		case T_CustomScan:
			children = castNode(CustomScan, plan)->custom_plans;
			break;
		default:
			break;
	}
	foreach(cell, children)
		bloompg_visit_plan((Plan *) lfirst(cell), graph);
}

BloomPGJoinGraph *
bloompg_graph_build(Query *query, MemoryContext context)
{
	BloomPGJoinGraph *graph;
	MemoryContext old_context;
	ListCell *cell;
	Index rti = 0;

	old_context = MemoryContextSwitchTo(context);
	graph = palloc0(sizeof(BloomPGJoinGraph));
	graph->memory_context = context;
	graph->rtable_count = list_length(query->rtable);
	graph->has_set_operation = query->setOperations != NULL;
	graph->has_recursive_scope = query->hasRecursive;
	foreach(cell, query->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, cell);
		BloomPGTable *table;

		rti++;
		if (rte->rtekind != RTE_RELATION && rte->rtekind != RTE_SUBQUERY &&
			rte->rtekind != RTE_CTE)
			continue;
		table = palloc0(sizeof(BloomPGTable));
		table->rti = rti;
		table->relation_oid = rte->rtekind == RTE_RELATION ? rte->relid : InvalidOid;
		table->rte_kind = rte->rtekind;
		table->alias = pstrdup(rte->eref != NULL ? rte->eref->aliasname : "?");
		table->subquery = rte->rtekind == RTE_SUBQUERY ? rte->subquery : NULL;
		if (rte->rtekind == RTE_RELATION)
			table->required_attributes =
				bloompg_query_required_attributes(query, rti);
		graph->tables = lappend(graph->tables, table);
	}
	bloompg_visit_jointree((Node *) query->jointree, graph);
	MemoryContextSwitchTo(old_context);
	return graph;
}

void
bloompg_graph_augment_plan(BloomPGJoinGraph *graph, PlannedStmt *statement)
{
	MemoryContext old_context;
	ListCell *cell;
	Index rti = 0;

	if (graph == NULL || statement == NULL)
		return;
	old_context = MemoryContextSwitchTo(graph->memory_context);
	foreach(cell, statement->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, cell);
		ListCell *table_cell;
		bool exists = false;

		rti++;
		foreach(table_cell, graph->tables)
		{
			BloomPGTable *table = lfirst(table_cell);

			if (table->rti == rti)
			{
				exists = true;
				break;
			}
		}
		if (exists ||
			(rte->rtekind != RTE_RELATION && rte->rtekind != RTE_SUBQUERY &&
			 rte->rtekind != RTE_CTE))
			continue;
		{
			BloomPGTable *table = palloc0(sizeof(BloomPGTable));

			table->rti = rti;
			table->relation_oid = rte->rtekind == RTE_RELATION ?
				rte->relid : InvalidOid;
			table->rte_kind = rte->rtekind;
			table->alias = pstrdup(rte->eref != NULL ?
				rte->eref->aliasname : "?");
			table->subquery = rte->rtekind == RTE_SUBQUERY ?
				rte->subquery : NULL;
			if (rte->rtekind == RTE_RELATION)
			{
				ListCell *source_cell;

				foreach(source_cell, graph->tables)
				{
					BloomPGTable *source = lfirst(source_cell);

					if (source->subquery == NULL)
						continue;
					table->required_attributes =
						bloompg_find_pulled_required_attributes(
							source->subquery, rte);
					if (table->required_attributes != NULL)
						break;
				}
			}
			graph->tables = lappend(graph->tables, table);
		}
	}
	graph->rtable_count = Max(graph->rtable_count,
		list_length(statement->rtable));
	bloompg_visit_plan(statement->planTree, graph);
	MemoryContextSwitchTo(old_context);
}

char *
bloompg_graph_trace(const BloomPGJoinGraph *graph)
{
	StringInfoData buffer;
	ListCell *cell;

	initStringInfo(&buffer);
	appendStringInfo(&buffer, "tables=%d edges=%d", list_length(graph->tables),
					 list_length(graph->edges));
	foreach(cell, graph->edges)
	{
		const BloomPGJoinEdge *edge = lfirst(cell);

		appendStringInfo(&buffer, " [%u.%d%s -> %u.%d%s op=%u]",
						 edge->left.rti, edge->left.attribute,
						 edge->protect_left ? " protected" : "",
						 edge->right.rti, edge->right.attribute,
						 edge->protect_right ? " protected" : "",
						 edge->operator_oid);
	}
	return buffer.data;
}
