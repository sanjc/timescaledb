/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>
#include <nodes/nodeFuncs.h>
#include <optimizer/clauses.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <optimizer/tlist.h>
#include <utils/builtins.h>
#include <utils/typcache.h>

#include "chunk_append/chunk_append.h"
#include "chunk_append/planner.h"
#include "compat.h"
#include "guc.h"

static bool contain_param_exec(Node *node);
static bool contain_param_exec_walker(Node *node, void *context);
static Var *find_equality_join_var(Var *sort_var, Index ht_relid, Oid eq_opr,
								   List *join_conditions);

static CustomPathMethods chunk_append_path_methods = {
	.CustomName = "ChunkAppend",
	.PlanCustomPath = chunk_append_plan_create,
};

Path *
ts_chunk_append_path_create(PlannerInfo *root, RelOptInfo *rel, Hypertable *ht, Path *subpath,
							bool ordered, List *nested_oids)
{
	ChunkAppendPath *path;
	ListCell *lc;
	double rows = 0.0;
	Cost total_cost = 0.0;
	List *children = NIL;

	path = (ChunkAppendPath *) newNode(sizeof(ChunkAppendPath), T_CustomPath);

	path->cpath.path.pathtype = T_CustomScan;
	path->cpath.path.parent = rel;
	path->cpath.path.pathtarget = rel->reltarget;
	path->cpath.path.param_info = subpath->param_info;

	path->cpath.path.parallel_aware = false;
	path->cpath.path.parallel_safe = subpath->parallel_safe;
	path->cpath.path.parallel_workers = subpath->parallel_workers;

	/*
	 * Set flags. We can set CUSTOMPATH_SUPPORT_BACKWARD_SCAN and
	 * CUSTOMPATH_SUPPORT_MARK_RESTORE. The only interesting flag is the first
	 * one (backward scan), but since we are not scanning a real relation we
	 * need not indicate that we support backward scans. Lower-level index
	 * scanning nodes will scan backward if necessary, so once tuples get to
	 * this node they will be in a given order already.
	 */
	path->cpath.flags = 0;
	path->cpath.methods = &chunk_append_path_methods;

	/*
	 * check if we should do startup and runtime exclusion
	 */
	foreach (lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (contain_mutable_functions((Node *) rinfo->clause))
			path->startup_exclusion = true;

		if (ts_guc_enable_runtime_exclusion && contain_param_exec((Node *) rinfo->clause))
			path->runtime_exclusion = true;
	}

	/*
	 * Make sure our subpath is either an Append or MergeAppend node
	 */
	switch (nodeTag(subpath))
	{
		case T_AppendPath:
			children = castNode(AppendPath, subpath)->subpaths;
			break;
		case T_MergeAppendPath:
			/*
			 * check if ordered append is applicable, only assert ordered here
			 * checked properly in ts_ordered_append_should_optimize
			 */
			Assert(ordered);

			/*
			 * we only push down LIMIT for ordered append
			 */
			path->pushdown_limit = true;
			children = castNode(MergeAppendPath, subpath)->subpaths;
			path->cpath.path.pathkeys = subpath->pathkeys;
			break;
		default:
			elog(ERROR, "invalid child of chunk append: %u", nodeTag(subpath));
			break;
	}

	if (!ordered || ht->space->num_dimensions == 1)
		path->cpath.custom_paths = children;
	else
	{
		/*
		 * For space partitioning we need to change the shape of the plan
		 * into a MergeAppend for each time slice with all space partitions below
		 * The final plan for space partitioning will look like this:
		 *
		 * Custom Scan (ChunkAppend)
		 *   Hypertable: space
		 *   ->  Merge Append
		 *         Sort Key: _hyper_9_56_chunk."time"
		 *         ->  Index Scan
		 *         ->  Index Scan
		 *         ->  Index Scan
		 *   ->  Merge Append
		 *         Sort Key: _hyper_9_55_chunk."time"
		 *         ->  Index Scan
		 *         ->  Index Scan
		 *         ->  Index Scan
		 *
		 * We do not check sort order at this stage but injecting of Sort
		 * nodes happens when the plan is created instead.
		 */
		ListCell *flat = list_head(children);
		List *nested_children = NIL;
		bool has_scan_childs = false;

		foreach (lc, nested_oids)
		{
			ListCell *lc_oid;
			List *current_oids = lfirst(lc);
			List *merge_childs = NIL;
			MergeAppendPath *append;

			foreach (lc_oid, current_oids)
			{
				Assert(lfirst_oid(lc_oid) ==
					   root->simple_rte_array[((Path *) lfirst(flat))->parent->relid]->relid);
				merge_childs = lappend(merge_childs, lfirst(flat));
				flat = lnext(flat);
			}

			if (list_length(merge_childs) > 1)
			{
#if PG96
				append = create_merge_append_path(root,
												  rel,
												  merge_childs,
												  path->cpath.path.pathkeys,
												  PATH_REQ_OUTER(subpath));
#else
				append = create_merge_append_path(root,
												  rel,
												  merge_childs,
												  path->cpath.path.pathkeys,
												  PATH_REQ_OUTER(subpath),
												  NIL);
#endif
				nested_children = lappend(nested_children, append);
			}
			else
			{
				has_scan_childs = true;
				nested_children = lappend(nested_children, linitial(merge_childs));
			}
		}

		/*
		 * if we do not have scans as direct childs of this
		 * node we disable startup and runtime exclusion
		 * in this node
		 */
		if (!has_scan_childs)
		{
			path->startup_exclusion = false;
			path->runtime_exclusion = false;
		}

		path->cpath.custom_paths = nested_children;
	}

	foreach (lc, path->cpath.custom_paths)
	{
		Path *child = lfirst(lc);

		/*
		 * If there is a LIMIT clause we only include as many chunks as
		 * planner thinks are needed to satisfy LIMIT clause.
		 * We do this to prevent planner choosing parallel plan which might
		 * otherwise look preferable cost wise.
		 */
		if (!path->pushdown_limit || root->limit_tuples == -1.0 || rows < root->limit_tuples)
		{
			total_cost += child->total_cost;
			rows += child->rows;
		}
	}

	path->cpath.path.rows = rows;
	path->cpath.path.total_cost = total_cost;

	if (path->cpath.custom_paths != NIL)
		path->cpath.path.startup_cost = ((Path *) linitial(path->cpath.custom_paths))->startup_cost;

	return &path->cpath.path;
}

/*
 * Check if conditions for doing ordered append optimization are fulfilled
 */
bool
ts_ordered_append_should_optimize(PlannerInfo *root, RelOptInfo *rel, Hypertable *ht,
								  List *join_conditions, int *order_attno, bool *reverse)
{
	SortGroupClause *sort = linitial(root->parse->sortClause);
	TargetEntry *tle = get_sortgroupref_tle(sort->tleSortGroupRef, root->parse->targetList);
	RangeTblEntry *rte = root->simple_rte_array[rel->relid];
	TypeCacheEntry *tce;
	char *column;
	Index ht_relid = rel->relid;
	Index sort_relid;
	Var *ht_var;
	Var *sort_var;

	/* these are checked in caller so we only Assert */
	Assert(!ts_guc_disable_optimizations && ts_guc_enable_ordered_append &&
		   ts_guc_enable_chunk_append);

	/*
	 * only do this optimization for queries with an ORDER BY clause,
	 * caller checked this, so only asserting
	 */
	Assert(root->parse->sortClause != NIL);

	/* we only support direct column references here */
	if (!IsA(tle->expr, Var))
		return false;

	sort_var = castNode(Var, tle->expr);
	sort_relid = sort_var->varno;
	tce = lookup_type_cache(sort_var->vartype,
							TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

	/* check sort operation is either less than or greater than */
	if (sort->sortop != tce->lt_opr && sort->sortop != tce->gt_opr)
		return false;

	/*
	 * check the ORDER BY column actually belongs to our hypertable
	 */
	if (sort_relid == ht_relid)
	{
		/* ORDER BY column belongs to our hypertable */
		ht_var = sort_var;
	}
	else
	{
		/*
		 * If the ORDER BY does not match our hypertable but we are joining
		 * against another hypertable on the time column doing an ordered
		 * append here is still beneficial because we can skip the sort
		 * step for the MergeJoin
		 */
		if (join_conditions == NIL)
			return false;

		ht_var = find_equality_join_var(sort_var, ht_relid, tce->eq_opr, join_conditions);

		if (ht_var == NULL)
			return false;
	}

	/* Check hypertable column is the first dimension of the hypertable */
	column = strVal(list_nth(rte->eref->colnames, AttrNumberGetAttrOffset(ht_var->varattno)));
	if (namestrcmp(&ht->space->dimensions[0].fd.column_name, column) != 0)
		return false;

	Assert(order_attno != NULL && reverse != NULL);
	*order_attno = ht_var->varattno;
	*reverse = sort->sortop == tce->lt_opr ? false : true;

	return true;
}

static bool
contain_param_exec(Node *node)
{
	return contain_param_exec_walker(node, NULL);
}

static bool
contain_param_exec_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Param))
		return castNode(Param, node)->paramkind == PARAM_EXEC;

	return expression_tree_walker(node, contain_param_exec_walker, context);
}

/*
 * Find equality join between column referenced by sort_var and Relation
 * with relid ht_relid
 */
static Var *
find_equality_join_var(Var *sort_var, Index ht_relid, Oid eq_opr, List *join_conditions)
{
	ListCell *lc;
	Index sort_relid = sort_var->varno;

	foreach (lc, join_conditions)
	{
		OpExpr *op = lfirst(lc);

		if (op->opno == eq_opr)
		{
			Var *left = linitial(op->args);
			Var *right = lsecond(op->args);

			Assert(IsA(left, Var) && IsA(right, Var));

			/* Is this a join condition referencing our hypertable */
			if ((left->varno == sort_relid && right->varno == ht_relid &&
				 left->varattno == sort_var->varattno) ||
				(left->varno == ht_relid && right->varno == sort_relid &&
				 right->varattno == sort_var->varattno))
				return left->varno == sort_relid ? right : left;
		}
	}

	return NULL;
}