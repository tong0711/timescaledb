/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>
#include <nodes/relation.h>
#include <parser/parsetree.h>
#include <optimizer/clauses.h>
#include <optimizer/var.h>
#include <optimizer/restrictinfo.h>
#include <nodes/plannodes.h>
#include <optimizer/prep.h>
#include <nodes/nodeFuncs.h>
#include <nodes/makefuncs.h>

#include <catalog/pg_constraint.h>
#include <catalog/pg_inherits.h>
#include <catalog/pg_namespace.h>
#include "compat.h"
#if PG96 || PG10 /* PG11 consolidates pg_foo_fn.h -> pg_foo.h */
#include <catalog/pg_constraint_fn.h>
#include <catalog/pg_inherits_fn.h>
#endif
#include <optimizer/pathnode.h>
#include <optimizer/tlist.h>
#include <catalog/pg_type.h>
#include <utils/errcodes.h>
#include <utils/syscache.h>

#include "plan_expand_hypertable.h"
#include "hypertable.h"
#include "hypertable_restrict_info.h"
#include "planner.h"
#include "planner_import.h"
#include "plan_ordered_append.h"
#include "guc.h"
#include "extension.h"
#include "chunk.h"
#include "extension_constants.h"
#include "partitioning.h"

typedef struct CollectQualCtx
{
	PlannerInfo *root;
	RelOptInfo *rel;
	List *restrictions;
	FuncExpr *chunk_exclusion_func;
} CollectQualCtx;

static Oid chunk_exclusion_func = InvalidOid;
#define CHUNK_EXCL_FUNC_NAME "chunks_in"
static Oid ts_chunks_arg_types[] = { RECORDOID, INT4ARRAYOID };

static void
init_chunk_exclusion_func()
{
	if (chunk_exclusion_func == InvalidOid)
		chunk_exclusion_func = get_function_oid(CHUNK_EXCL_FUNC_NAME,
												INTERNAL_SCHEMA_NAME,
												lengthof(ts_chunks_arg_types),
												ts_chunks_arg_types);
	Assert(chunk_exclusion_func != InvalidOid);
}

static bool
is_chunk_exclusion_func(Expr *node)
{
	if (IsA(node, FuncExpr) && castNode(FuncExpr, node)->funcid == chunk_exclusion_func)
		return true;

	return false;
}

static bool
is_time_bucket_function(Expr *node)
{
	if (IsA(node, FuncExpr) &&
		strncmp(get_func_name(castNode(FuncExpr, node)->funcid), "time_bucket", NAMEDATALEN) == 0)
		return true;

	return false;
}

/*
 * Transform time_bucket calls of the following form in WHERE clause:
 *
 * time_bucket(width, column) OP value
 *
 * Since time_bucket always returns the lower bound of the bucket
 * for lower bound comparisons the width is not relevant and the
 * following transformation can be applied:
 *
 * time_bucket(width, column) > value
 * column > value
 *
 * Example with values:
 *
 * time_bucket(10, column) > 109
 * column > 109
 *
 * For upper bound comparisons width needs to be taken into account
 * and we need to extend the upper bound by width to capture all
 * possible values.
 *
 * time_bucket(width, column) < value
 * column < value + width
 *
 * Example with values:
 *
 * time_bucket(10, column) < 100
 * column < 100 + 10
 *
 * Expressions with value on the left side will be switched around
 * when building the expression for RestrictInfo.
 *
 * Caller must ensure that only 2 argument time_bucket versions
 * are used.
 */
static OpExpr *
transform_time_bucket_comparison(PlannerInfo *root, OpExpr *op)
{
	Expr *left = linitial(op->args);
	Expr *right = lsecond(op->args);

	FuncExpr *time_bucket = castNode(FuncExpr, (IsA(left, FuncExpr) ? left : right));
	Expr *value = IsA(right, Const) ? right : left;

	Const *width = linitial(time_bucket->args);
	Oid opno = op->opno;
	TypeCacheEntry *tce;
	int strategy;

	/* caller must ensure time_bucket only has 2 arguments */
	Assert(list_length(time_bucket->args) == 2);

	/*
	 * if time_bucket call is on wrong side we switch operator
	 */
	if (IsA(right, FuncExpr))
	{
		opno = get_commutator(op->opno);

		if (!OidIsValid(opno))
			return op;
	}

	tce = lookup_type_cache(exprType((Node *) time_bucket), TYPECACHE_BTREE_OPFAMILY);
	strategy = get_op_opfamily_strategy(opno, tce->btree_opf);

	if (strategy == BTGreaterStrategyNumber || strategy == BTGreaterEqualStrategyNumber)
	{
		/* column > value */
		op = copyObject(op);
		op->args = list_make2(lsecond(time_bucket->args), value);

		/*
		 * if we switched operator we need to adjust OpExpr as well
		 */
		if (IsA(right, FuncExpr))
		{
			op->opno = opno;
			op->opfuncid = InvalidOid;
		}

		return op;
	}
	else if (strategy == BTLessStrategyNumber || strategy == BTLessEqualStrategyNumber)
	{
		/* column < value + width */
		Expr *subst;
		Oid resulttype;
		Oid subst_opno = get_operator("+",
									  PG_CATALOG_NAMESPACE,
									  exprType((Node *) value),
									  exprType((Node *) width));

		if (!OidIsValid(subst_opno))
			return op;

		if (tce->type_id == TIMESTAMPTZOID && width->consttype == INTERVALOID &&
			DatumGetIntervalP(width)->month == 0 && DatumGetIntervalP(width)->day != 0)
		{
			/*
			 * If width interval has day component we merge it with
			 * time component because estimating the day component
			 * depends on the session timezone and that would be
			 * unsafe during planning time.
			 * But since time_bucket calculation always is relative
			 * to UTC it is safe to do this transformation and assume
			 * day to always be 24 hours.
			 */
			Interval *interval;

			width = copyObject(width);
			interval = DatumGetIntervalP(width->constvalue);
			interval->time += interval->day * USECS_PER_DAY;
			interval->day = 0;
		}

		resulttype = get_op_rettype(subst_opno);
		subst = make_opclause(subst_opno,
							  tce->type_id,
							  false,
							  value,
							  (Expr *) width,
							  InvalidOid,
							  InvalidOid);

		/*
		 * check if resulttype of operation returns correct datatype
		 *
		 * date OP interval returns timestamp so we need to insert
		 * a cast to keep toplevel expr intact when datatypes don't match
		 */
		if (tce->type_id != resulttype)
		{
			Oid cast_func = get_cast_func(resulttype, tce->type_id);

			if (!OidIsValid(cast_func))
				return op;

			subst = (Expr *)
				makeFuncExpr(cast_func, tce->type_id, list_make1(subst), InvalidOid, InvalidOid, 0);
		}

		if (tce->type_id == TIMESTAMPTZOID && width->consttype == INTERVALOID)
		{
			/*
			 * TIMESTAMPTZ OP INTERVAL is marked stable and unsafe
			 * to evaluate at plan time unless it only has a time
			 * component
			 */
			Interval *interval = DatumGetIntervalP(width->constvalue);

			if (interval->day == 0 && interval->month == 0)
				subst = (Expr *) estimate_expression_value(root, (Node *) subst);
		}

		op = copyObject(op);

		/*
		 * if we switched operator we need to adjust OpExpr as well
		 */
		if (IsA(right, FuncExpr))
		{
			op->opno = opno;
			op->opfuncid = InvalidOid;
		}

		op->args = list_make2(lsecond(time_bucket->args), subst);
	}

	return op;
}

/* Since baserestrictinfo is not yet set by the planner, we have to derive
 * it ourselves. It's safe for us to miss some restrict info clauses (this
 * will just result in more chunks being included) so this does not need
 * to be as comprehensive as the PG native derivation. This is inspired
 * by the derivation in `deconstruct_recurse` in PG
 *
 * When we detect explicit chunk exclusion with the chunks_in function
 * we stop further processing and do an early exit.
 *
 * This function removes chunks_in from the list of quals, because chunks_in is
 * just used as marker function to trigger explicit chunk exclusion and the function
 * will throw an error when executed.
 */
static Node *
process_quals(Node *quals, CollectQualCtx *ctx)
{
	ListCell *lc;
	ListCell *prev = NULL;

	for (lc = list_head((List *) quals); lc != NULL; prev = lc, lc = lnext(lc))
	{
		Expr *qual = lfirst(lc);
		RestrictInfo *restrictinfo;
		Relids relids = pull_varnos((Node *) qual);

		/*
		 * skip expressions not for current rel
		 */
		if (bms_num_members(relids) != 1 || !bms_is_member(ctx->rel->relid, relids))
			continue;

		if (is_chunk_exclusion_func(qual))
		{
			FuncExpr *func_expr = (FuncExpr *) qual;

			/* validation */
			Assert(func_expr->args->length == 2);
			if (!IsA(linitial(func_expr->args), Var))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("first parameter for chunks_in function needs to be record")));

			ctx->chunk_exclusion_func = func_expr;
			ctx->restrictions = NIL;
			quals = (Node *) list_delete_cell((List *) quals, lc, prev);
			return quals;
		}

		if (IsA(qual, OpExpr) && list_length(castNode(OpExpr, qual)->args) == 2)
		{
			OpExpr *op = castNode(OpExpr, qual);
			Expr *left = linitial(op->args);
			Expr *right = lsecond(op->args);

			if ((IsA(left, FuncExpr) && IsA(right, Const) &&
				 list_length(castNode(FuncExpr, left)->args) == 2 &&
				 is_time_bucket_function(left)) ||
				(IsA(left, Const) && IsA(right, FuncExpr) &&
				 list_length(castNode(FuncExpr, right)->args) == 2 &&
				 is_time_bucket_function(right)))
			{
				qual = (Expr *) transform_time_bucket_comparison(ctx->root, op);
			}
		}

#if PG96
		restrictinfo = make_restrictinfo(qual, true, false, false, relids, NULL, NULL);
#else
		restrictinfo = make_restrictinfo(qual,
										 true,
										 false,
										 false,
										 ctx->root->qual_security_level,
										 relids,
										 NULL,
										 NULL);
#endif
		ctx->restrictions = lappend(ctx->restrictions, restrictinfo);
	}
	return quals;
}

static bool
collect_quals_walker(Node *node, CollectQualCtx *ctx)
{
	if (node == NULL)
		return false;

	if (IsA(node, FromExpr))
	{
		FromExpr *f = castNode(FromExpr, node);
		f->quals = process_quals(f->quals, ctx);
	}
	else if (IsA(node, JoinExpr))
	{
		JoinExpr *j = castNode(JoinExpr, node);
		j->quals = process_quals(j->quals, ctx);
	}

	/* skip processing if we found a chunks_in call for current relation */
	if (ctx->chunk_exclusion_func != NULL)
		return true;

	return expression_tree_walker(node, collect_quals_walker, ctx);
}

static List *
find_children_oids(HypertableRestrictInfo *hri, Hypertable *ht, LOCKMODE lockmode)
{
	/*
	 * Using the HRI only makes sense if we are not using all the chunks,
	 * otherwise using the cached inheritance hierarchy is faster.
	 */
	if (!ts_hypertable_restrict_info_has_restrictions(hri))
		return find_inheritance_children(ht->main_table_relid, lockmode);

	/*
	 * Unlike find_all_inheritors we do not include parent because if there
	 * are restrictions the parent table cannot fulfill them and since we do
	 * have a trigger blocking inserts on the parent table it cannot contain
	 * any rows.
	 */
	return ts_hypertable_restrict_info_get_chunk_oids(hri, ht, lockmode);
}

static bool
should_order_append(PlannerInfo *root, RelOptInfo *rel, Hypertable *ht, bool *reverse)
{
	/* check if optimizations are enabled */
	if (ts_guc_disable_optimizations || !ts_guc_enable_ordered_append)
		return false;

	/*
	 * only do this optimization for hypertables with 1 dimension and queries
	 * with an ORDER BY and LIMIT clause
	 */
	if (ht->space->num_dimensions != 1 || root->parse->sortClause == NIL ||
		root->limit_tuples == -1.0)
		return false;

	return ts_ordered_append_should_optimize(root, rel, ht, reverse);
}

bool
ts_plan_expand_hypertable_valid_hypertable(Hypertable *ht, Query *parse, Index rti,
										   RangeTblEntry *rte)
{
	if (ht == NULL ||
		/* inheritance enabled */
		rte->inh == false ||
		/* row locks not necessary */
		parse->rowMarks != NIL ||
		/* not update and/or delete */
		0 != parse->resultRelation)
		return false;

	return true;
}

/*  get chunk oids specified by explicit chunk exclusion function */
static List *
get_explicit_chunk_oids(CollectQualCtx *ctx, Hypertable *ht)
{
	List *chunk_oids = NIL;
	Const *chunks_arg;
	ArrayIterator chunk_id_iterator;
	Datum elem = (Datum) NULL;
	bool isnull;
	Expr *expr;

	Assert(ctx->chunk_exclusion_func->args->length == 2);
	expr = lsecond(ctx->chunk_exclusion_func->args);
	if (!IsA(expr, Const))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("second argument to chunk_in should contain only integer consts")));

	chunks_arg = (Const *) expr;

	/* function marked as STRICT so argument can't be NULL */
	Assert(!chunks_arg->constisnull);

	chunk_id_iterator = array_create_iterator(DatumGetArrayTypeP(chunks_arg->constvalue), 0, NULL);

	while (array_iterate(chunk_id_iterator, &elem, &isnull))
	{
		if (!isnull)
		{
			int32 chunk_id = DatumGetInt32(elem);
			Chunk *chunk = ts_chunk_get_by_id(chunk_id, 0, false);

			if (chunk == NULL)
				ereport(ERROR, (errmsg("chunk id %d not found", chunk_id)));

			if (chunk->fd.hypertable_id != ht->fd.id)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("chunk id %d does not belong to hypertable \"%s\"",
								chunk_id,
								NameStr(ht->fd.table_name))));

			chunk_oids = lappend_int(chunk_oids, chunk->table_id);
		}
		else
			elog(ERROR, "chunk id can't be NULL");
	}
	array_free_iterator(chunk_id_iterator);
	return chunk_oids;
}

/**
 * Get chunk oids from either restrict info or explicit chunk exclusion. Explicit chunk exclusion
 * takes precedence.
 */
static List *
get_chunk_oids(CollectQualCtx *ctx, PlannerInfo *root, RelOptInfo *rel, Hypertable *ht)
{
	bool reverse;

	if (ctx->chunk_exclusion_func == NULL)
	{
		HypertableRestrictInfo *hri = ts_hypertable_restrict_info_create(rel, ht);

		/*
		 * This is where the magic happens: use our HypertableRestrictInfo
		 * infrastructure to deduce the appropriate chunks using our range
		 * exclusion
		 */
		ts_hypertable_restrict_info_add(hri, root, ctx->restrictions);

		if (should_order_append(root, rel, ht, &reverse))
		{
			if (rel->fdw_private != NULL)
				((TimescaleDBPrivate *) rel->fdw_private)->appends_ordered = true;
			return ts_hypertable_restrict_info_get_chunk_oids_ordered(hri,
																	  ht,
																	  AccessShareLock,
																	  reverse);
		}
		else
			return find_children_oids(hri, ht, AccessShareLock);
	}
	else
		return get_explicit_chunk_oids(ctx, ht);
}

#if !(PG96 || PG10)

/*
 * Create partition expressions for a hypertable.
 *
 * Build an array of partition expressions where each element represents valid
 * expressions on a particular partitioning key.
 *
 * The partition expressions are used by, e.g., group_by_has_partkey() to check
 * whether a GROUP BY clause covers all partitioning dimensions.
 *
 * For dimensions with a partitioning function, we can support either
 * expressions on the plain key (column) or the partitioning function applied
 * to the key. For instance, the queries
 *
 * SELECT time, device, avg(temp)
 * FROM hypertable
 * GROUP BY 1, 2;
 *
 * and
 *
 * SELECT time_func(time), device, avg(temp)
 * FROM hypertable
 * GROUP BY 1, 2;
 *
 * are both amenable to aggregate push down if "time" is supported by the
 * partitioning function "time_func" and "device" is also a partitioning
 * dimension.
 */
static List **
get_hypertable_partexprs(Hypertable *ht, Query *parse, Index varno)
{
	int i;
	List **partexprs;

	Assert(NULL != ht->space);

	partexprs = palloc0(sizeof(List *) * ht->space->num_dimensions);

	for (i = 0; i < ht->space->num_dimensions; i++)
	{
		Dimension *dim = &ht->space->dimensions[i];
		Expr *expr;
		HeapTuple tuple = SearchSysCacheAttNum(ht->main_table_relid, dim->column_attno);
		Form_pg_attribute att;

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for attribute");

		att = (Form_pg_attribute) GETSTRUCT(tuple);

		expr = (Expr *)
			makeVar(varno, dim->column_attno, att->atttypid, att->atttypmod, att->attcollation, 0);

		ReleaseSysCache(tuple);

		/* The expression on the partitioning key can be the raw key or the
		 * partitioning function on the key */
		if (NULL != dim->partitioning)
			partexprs[i] = list_make2(expr, dim->partitioning->partfunc.func_fmgr.fn_expr);
		else
			partexprs[i] = list_make1(expr);
	}

	return partexprs;
}

#define PARTITION_STRATEGY_MULTIDIM 'm'

/*
 * Partition info for hypertables.
 *
 * Build a "fake" partition scheme for a hypertable that makes the planner
 * believe this is a PostgreSQL partitioned table for planning purposes. In
 * particular, this will make the planner consider partitionwise aggregations
 * when applicable.
 *
 * Partitionwise aggregation can either be FULL or PARTIAL. The former means
 * that the aggregation can be performed independently on each partition
 * (chunk) without a finalize step which is needed in PARTIAL. FULL requires
 * that the GROUP BY clause contains all hypertable partitioning
 * dimensions. This requirement is enforced by creating a partitioning scheme
 * that covers multiple attributes, i.e., one per dimension. This works well
 * since the "shallow" (one-level hierarchy) of a multi-dimensional hypertable
 * is similar to a one-level partitioned PostgreSQL table where the
 * partitioning key covers multiple attributes.
 *
 * Note that we use a partition scheme with a strategy that does not exist in
 * PostgreSQL. This makes PostgreSQL raise errors when this partition scheme is
 * used in places that require a valid partition scheme with a supported
 * strategy.
 */
static void
build_hypertable_partition_info(Hypertable *ht, PlannerInfo *root, RelOptInfo *hyper_rel,
								int nparts)
{
	PartitionScheme part_scheme = palloc0(sizeof(PartitionSchemeData));

	/* We only set the info needed for planning */
	part_scheme->partnatts = ht->space->num_dimensions;
	part_scheme->strategy = PARTITION_STRATEGY_MULTIDIM;
	hyper_rel->nparts = nparts;
	hyper_rel->part_scheme = part_scheme;
	hyper_rel->partexprs = get_hypertable_partexprs(ht, root->parse, hyper_rel->relid);
	hyper_rel->nullable_partexprs = (List **) palloc0(sizeof(List *) * part_scheme->partnatts);
}

#endif /* !(PG96 || PG10) */

/* Inspired by expand_inherited_rtentry but expands
 * a hypertable chunks into an append relationship */
void
ts_plan_expand_hypertable_chunks(Hypertable *ht, PlannerInfo *root, Oid parent_oid, bool inhparent,
								 RelOptInfo *rel)
{
	RangeTblEntry *rte = rt_fetch(rel->relid, root->parse->rtable);
	List *inh_oids;
	ListCell *l;
	Relation oldrelation = heap_open(parent_oid, NoLock);
	Query *parse = root->parse;
	Index rti = rel->relid;
	List *appinfos = NIL;
	PlanRowMark *oldrc;
	CollectQualCtx ctx = {
		.root = root,
		.rel = rel,
		.restrictions = NIL,
		.chunk_exclusion_func = NULL,
	};

	/* double check our permissions are valid */
	Assert(rti != parse->resultRelation);
	oldrc = get_plan_rowmark(root->rowMarks, rti);
	if (oldrc && RowMarkRequiresRowShareLock(oldrc->markType))
		elog(ERROR, "unexpected permissions requested");

	/* mark the parent as an append relation */
	rte->inh = true;

	init_chunk_exclusion_func();

	/* Walk the tree and find restrictions or chunk exclusion functions */
	collect_quals_walker((Node *) root->parse->jointree, &ctx);

	inh_oids = get_chunk_oids(&ctx, root, rel, ht);

	/*
	 * the simple_*_array structures have already been set, we need to add the
	 * children to them
	 */
	root->simple_rel_array_size += list_length(inh_oids);
	root->simple_rel_array =
		repalloc(root->simple_rel_array, root->simple_rel_array_size * sizeof(RelOptInfo *));
	root->simple_rte_array =
		repalloc(root->simple_rte_array, root->simple_rel_array_size * sizeof(RangeTblEntry *));

#if !(PG96 || PG10)
	/* Adding partition info will make PostgreSQL consider the inheritance
	 * children as part of a partitioned relation. This will enable
	 * partitionwise aggregation. */
	build_hypertable_partition_info(ht, root, rel, list_length(inh_oids));
#endif

	foreach (l, inh_oids)
	{
		Oid child_oid = lfirst_oid(l);
		Relation newrelation;
		RangeTblEntry *childrte;
		Index child_rtindex;
		AppendRelInfo *appinfo;

		/* Open rel if needed; we already have required locks */
		if (child_oid != parent_oid)
			newrelation = heap_open(child_oid, NoLock);
		else
			newrelation = oldrelation;

		/* chunks cannot be temp tables */
		Assert(!RELATION_IS_OTHER_TEMP(newrelation));

		/*
		 * Build an RTE for the child, and attach to query's rangetable list.
		 * We copy most fields of the parent's RTE, but replace relation OID
		 * and relkind, and set inh = false.  Also, set requiredPerms to zero
		 * since all required permissions checks are done on the original RTE.
		 * Likewise, set the child's securityQuals to empty, because we only
		 * want to apply the parent's RLS conditions regardless of what RLS
		 * properties individual children may have.  (This is an intentional
		 * choice to make inherited RLS work like regular permissions checks.)
		 * The parent securityQuals will be propagated to children along with
		 * other base restriction clauses, so we don't need to do it here.
		 */
		childrte = copyObject(rte);
		childrte->relid = child_oid;
		childrte->relkind = newrelation->rd_rel->relkind;
		childrte->inh = false;
		/* clear the magic bit */
		childrte->ctename = NULL;
		childrte->requiredPerms = 0;
		childrte->securityQuals = NIL;
		parse->rtable = lappend(parse->rtable, childrte);
		child_rtindex = list_length(parse->rtable);
		root->simple_rte_array[child_rtindex] = childrte;
		root->simple_rel_array[child_rtindex] = NULL;

#if !PG96
		Assert(childrte->relkind != RELKIND_PARTITIONED_TABLE);
#endif

		appinfo = makeNode(AppendRelInfo);
		appinfo->parent_relid = rti;
		appinfo->child_relid = child_rtindex;
		appinfo->parent_reltype = oldrelation->rd_rel->reltype;
		appinfo->child_reltype = newrelation->rd_rel->reltype;
		ts_make_inh_translation_list(oldrelation,
									 newrelation,
									 child_rtindex,
									 &appinfo->translated_vars);
		appinfo->parent_reloid = parent_oid;
		appinfos = lappend(appinfos, appinfo);

		/* Close child relations, but keep locks */
		if (child_oid != parent_oid)
			heap_close(newrelation, NoLock);
	}

	heap_close(oldrelation, NoLock);

	root->append_rel_list = list_concat(root->append_rel_list, appinfos);
#if !PG96 && !PG10
	/*
	 * PG11 introduces a separate array to make looking up children faster, see:
	 * https://github.com/postgres/postgres/commit/7d872c91a3f9d49b56117557cdbb0c3d4c620687.
	 */
	setup_append_rel_array(root);
#endif
}
