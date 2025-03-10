/*
 * Tencent is pleased to support the open source community by making TBase available.  
 * 
 * Copyright (C) 2019 THL A29 Limited, a Tencent company.  All rights reserved.
 * 
 * TBase is licensed under the BSD 3-Clause License, except for the third-party component listed below. 
 * 
 * A copy of the BSD 3-Clause License is included in this file.
 * 
 * Other dependencies and licenses:
 * 
 * Open Source Software Licensed Under the PostgreSQL License: 
 * --------------------------------------------------------------------
 * 1. Postgres-XL XL9_5_STABLE
 * Portions Copyright (c) 2015-2016, 2ndQuadrant Ltd
 * Portions Copyright (c) 2012-2015, TransLattice, Inc.
 * Portions Copyright (c) 2010-2017, Postgres-XC Development Group
 * Portions Copyright (c) 1996-2015, The PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, The Regents of the University of California
 * 
 * Terms of the PostgreSQL License: 
 * --------------------------------------------------------------------
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 * 
 * IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * 
 * THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 * 
 * 
 * Terms of the BSD 3-Clause License:
 * --------------------------------------------------------------------
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation 
 * and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of THL A29 Limited nor the names of its contributors may be used to endorse or promote products derived from this software without 
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS 
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE 
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH 
 * DAMAGE.
 * 
 */
/*-------------------------------------------------------------------------
 *
 * analyze.c
 *      transform the raw parse tree into a query tree
 *
 * For optimizable statements, we are careful to obtain a suitable lock on
 * each referenced table, and other modules of the backend preserve or
 * re-obtain these locks before depending on the results.  It is therefore
 * okay to do significant semantic analysis of these statements.  For
 * utility commands, no locks are obtained here (and if they were, we could
 * not be sure we'd still have them at execution).  Hence the general rule
 * for utility commands is to just dump them into a Query node untransformed.
 * DECLARE CURSOR, EXPLAIN, and CREATE TABLE AS are exceptions because they
 * contain optimizable statements, which we should transform.
 *
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *    src/backend/parser/analyze.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"
#ifdef XCP
#include "catalog/pg_namespace.h"
#include "catalog/namespace.h"
#include "utils/builtins.h"
#endif
#ifdef PGXC
#include "catalog/pg_inherits.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/indexing.h"
#include "utils/tqual.h"
#endif
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_cte.h"
#include "parser/parse_oper.h"
#include "parser/parse_param.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parsetree.h"
#ifdef _MLS_
#include "postmaster/postmaster.h"
#endif
#include "rewrite/rewriteManip.h"
#ifdef PGXC
#include "miscadmin.h"
#include "pgxc/pgxc.h"
#include "pgxc/pgxcnode.h"
#include "access/gtm.h"
#include "utils/lsyscache.h"
#include "pgxc/planner.h"
#include "tcop/tcopprot.h"
#include "nodes/nodes.h"
#include "pgxc/poolmgr.h"
#include "catalog/pgxc_node.h"
#include "pgxc/xc_maintenance_mode.h"
#include "access/xact.h"
#endif
#include "utils/rel.h"
#ifdef _MLS_
#include "utils/datamask.h"
#endif
#ifdef __TBASE__
#include "optimizer/pgxcship.h"
#include "audit/audit_fga.h"
#include "pgstat.h"
#include "optimizer/clauses.h"
#include "utils/memutils.h"
#endif


#ifdef __TBASE__
/* GUC to enable transform insert into multi-values to copy from */
bool   g_transform_insert_to_copy;
#endif

/* Hook for plugins to get control at end of parse analysis */
post_parse_analyze_hook_type post_parse_analyze_hook = NULL;

static Query *transformOptionalSelectInto(ParseState *pstate, Node *parseTree);
static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, InsertStmt *stmt);
static List *transformInsertRow(ParseState *pstate, List *exprlist,
                   List *stmtcols, List *icolumns, List *attrnos,
                   bool strip_indirection);
static OnConflictExpr *transformOnConflictClause(ParseState *pstate,
                          OnConflictClause *onConflictClause);
static int    count_rowexpr_columns(ParseState *pstate, Node *expr);
static Query *transformSelectStmt(ParseState *pstate, SelectStmt *stmt);
static Query *transformValuesClause(ParseState *pstate, SelectStmt *stmt);
static Query *transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt);
static Node *transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
                          bool isTopLevel, List **targetlist);
static void determineRecursiveColTypes(ParseState *pstate,
                           Node *larg, List *nrtargetlist);
static Query *transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt);
static List *transformReturningList(ParseState *pstate, List *returningList);
static List *transformUpdateTargetList(ParseState *pstate,
                          List *targetList);
static Query *transformDeclareCursorStmt(ParseState *pstate,
                           DeclareCursorStmt *stmt);
static Query *transformExplainStmt(ParseState *pstate,
                     ExplainStmt *stmt);
static Query *transformCreateTableAsStmt(ParseState *pstate,
                           CreateTableAsStmt *stmt);
#ifdef PGXC
static Query *transformExecDirectStmt(ParseState *pstate, ExecDirectStmt *stmt);
#endif

static void transformLockingClause(ParseState *pstate, Query *qry,
                       LockingClause *lc, bool pushedDown);
#ifdef RAW_EXPRESSION_COVERAGE_TEST
static bool test_raw_expression_coverage(Node *node, void *context);
#endif

/*
 * parse_analyze
 *        Analyze a raw parse tree and transform it to Query form.
 *
 * Optionally, information about $n parameter types can be supplied.
 * References to $n indexes not defined by paramTypes[] are disallowed.
 *
 * The result is a Query node.  Optimizable statements require considerable
 * transformation, while utility-type statements are simply hung off
 * a dummy CMD_UTILITY Query node.
 */
Query *
parse_analyze(RawStmt *parseTree, const char *sourceText,
              Oid *paramTypes, int numParams,
              QueryEnvironment *queryEnv)
{
    ParseState *pstate = make_parsestate(NULL);
    Query       *query;

    Assert(sourceText != NULL); /* required as of 8.4 */

    pstate->p_sourcetext = sourceText;

    if (numParams > 0)
        parse_fixed_parameters(pstate, paramTypes, numParams);

    pstate->p_queryEnv = queryEnv;

    query = transformTopLevelStmt(pstate, parseTree);

    if (post_parse_analyze_hook)
        (*post_parse_analyze_hook) (pstate, query);

    free_parsestate(pstate);

    return query;
}

/*
 * parse_analyze_varparams
 *
 * This variant is used when it's okay to deduce information about $n
 * symbol datatypes from context.  The passed-in paramTypes[] array can
 * be modified or enlarged (via repalloc).
 */
Query *
parse_analyze_varparams(RawStmt *parseTree, const char *sourceText,
                        Oid **paramTypes, int *numParams)
{
    ParseState *pstate = make_parsestate(NULL);
    Query       *query;

    Assert(sourceText != NULL); /* required as of 8.4 */

    pstate->p_sourcetext = sourceText;

    parse_variable_parameters(pstate, paramTypes, numParams);

    query = transformTopLevelStmt(pstate, parseTree);

    /* make sure all is well with parameter types */
    check_variable_parameters(pstate, query);

    if (post_parse_analyze_hook)
        (*post_parse_analyze_hook) (pstate, query);

    free_parsestate(pstate);

    return query;
}

/*
 * parse_sub_analyze
 *        Entry point for recursively analyzing a sub-statement.
 */
Query *
parse_sub_analyze(Node *parseTree, ParseState *parentParseState,
                  CommonTableExpr *parentCTE,
                  bool locked_from_parent,
                  bool resolve_unknowns)
{
    ParseState *pstate = make_parsestate(parentParseState);
    Query       *query;

    pstate->p_parent_cte = parentCTE;
    pstate->p_locked_from_parent = locked_from_parent;
    pstate->p_resolve_unknowns = resolve_unknowns;

    query = transformStmt(pstate, parseTree);

    free_parsestate(pstate);

    return query;
}

/*
 * transformTopLevelStmt -
 *      transform a Parse tree into a Query tree.
 *
 * This function is just responsible for transferring statement location data
 * from the RawStmt into the finished Query.
 */
Query *
transformTopLevelStmt(ParseState *pstate, RawStmt *parseTree)
{
    Query       *result;

    /* We're at top level, so allow SELECT INTO */
    result = transformOptionalSelectInto(pstate, parseTree->stmt);

    result->stmt_location = parseTree->stmt_location;
    result->stmt_len = parseTree->stmt_len;

    return result;
}

/*
 * transformOptionalSelectInto -
 *      If SELECT has INTO, convert it to CREATE TABLE AS.
 *
 * The only thing we do here that we don't do in transformStmt() is to
 * convert SELECT ... INTO into CREATE TABLE AS.  Since utility statements
 * aren't allowed within larger statements, this is only allowed at the top
 * of the parse tree, and so we only try it before entering the recursive
 * transformStmt() processing.
 */
static Query *
transformOptionalSelectInto(ParseState *pstate, Node *parseTree)
{
    if (IsA(parseTree, SelectStmt))
    {
        SelectStmt *stmt = (SelectStmt *) parseTree;

        /* If it's a set-operation tree, drill down to leftmost SelectStmt */
        while (stmt && stmt->op != SETOP_NONE)
            stmt = stmt->larg;
        Assert(stmt && IsA(stmt, SelectStmt) &&stmt->larg == NULL);

        if ((stmt != NULL) && (stmt->intoClause))
        {
            CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);

            ctas->query = parseTree;
            ctas->into = stmt->intoClause;
            ctas->relkind = OBJECT_TABLE;
            ctas->is_select_into = true;

            /*
             * Remove the intoClause from the SelectStmt.  This makes it safe
             * for transformSelectStmt to complain if it finds intoClause set
             * (implying that the INTO appeared in a disallowed place).
             */
            stmt->intoClause = NULL;

            parseTree = (Node *) ctas;
        }
    }

    return transformStmt(pstate, parseTree);
}

/*
 * transformStmt -
 *      recursively transform a Parse tree into a Query tree.
 */
Query *
transformStmt(ParseState *pstate, Node *parseTree)
{// #lizard forgives
    Query       *result;

    /*
     * We apply RAW_EXPRESSION_COVERAGE_TEST testing to basic DML statements;
     * we can't just run it on everything because raw_expression_tree_walker()
     * doesn't claim to handle utility statements.
     */
#ifdef RAW_EXPRESSION_COVERAGE_TEST
    switch (nodeTag(parseTree))
    {
        case T_SelectStmt:
        case T_InsertStmt:
        case T_UpdateStmt:
        case T_DeleteStmt:
            (void) test_raw_expression_coverage(parseTree, NULL);
            break;
        default:
            break;
    }
#endif                            /* RAW_EXPRESSION_COVERAGE_TEST */

    switch (nodeTag(parseTree))
    {
            /*
             * Optimizable statements
             */
        case T_InsertStmt:
            result = transformInsertStmt(pstate, (InsertStmt *) parseTree);
            break;

        case T_DeleteStmt:
            result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
            break;

        case T_UpdateStmt:
            result = transformUpdateStmt(pstate, (UpdateStmt *) parseTree);
            break;

        case T_SelectStmt:
            {
                SelectStmt *n = (SelectStmt *) parseTree;

                if (n->valuesLists)
                    result = transformValuesClause(pstate, n);
                else if (n->op == SETOP_NONE)
                    result = transformSelectStmt(pstate, n);
                else
                    result = transformSetOperationStmt(pstate, n);
            }
            break;

            /*
             * Special cases
             */
        case T_DeclareCursorStmt:
            result = transformDeclareCursorStmt(pstate,
                                                (DeclareCursorStmt *) parseTree);
            break;

        case T_ExplainStmt:
            result = transformExplainStmt(pstate,
                                          (ExplainStmt *) parseTree);
            break;

#ifdef PGXC
        case T_ExecDirectStmt:
            result = transformExecDirectStmt(pstate,
                                             (ExecDirectStmt *) parseTree);
            break;
#endif

        case T_CreateTableAsStmt:
            result = transformCreateTableAsStmt(pstate,
                                                (CreateTableAsStmt *) parseTree);
            break;

        default:

            /*
             * other statements don't require any transformation; just return
             * the original parsetree with a Query node plastered on top.
             */
            result = makeNode(Query);
            result->commandType = CMD_UTILITY;
            result->utilityStmt = (Node *) parseTree;
            break;
    }

    /* Mark as original query until we learn differently */
    result->querySource = QSRC_ORIGINAL;
    result->canSetTag = true;

    return result;
}

/*
 * analyze_requires_snapshot
 *        Returns true if a snapshot must be set before doing parse analysis
 *        on the given raw parse tree.
 *
 * Classification here should match transformStmt().
 */
bool
analyze_requires_snapshot(RawStmt *parseTree)
{// #lizard forgives
    bool        result;

    switch (nodeTag(parseTree->stmt))
    {
            /*
             * Optimizable statements
             */
        case T_InsertStmt:
        case T_DeleteStmt:
        case T_UpdateStmt:
        case T_SelectStmt:
            result = true;
            break;

            /*
             * Special cases
             */
        case T_DeclareCursorStmt:
        case T_ExplainStmt:
        case T_CreateTableAsStmt:
            /* yes, because we must analyze the contained statement */
            result = true;
            break;

#ifdef PGXC
        case T_ExecDirectStmt:

            /*
             * We will parse/analyze/plan inner query, which probably will
             * need a snapshot. Ensure it is set.
             */
            result = true;
            break;
#endif

        default:
            /* other utility statements don't have any real parse analysis */
            result = false;
            break;
    }

    return result;
}

/*
 * transformDeleteStmt -
 *      transforms a Delete Statement
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
    Query       *qry = makeNode(Query);
    ParseNamespaceItem *nsitem;
    Node       *qual;

    qry->commandType = CMD_DELETE;

    /* process the WITH clause independently of all else */
    if (stmt->withClause)
    {
        qry->hasRecursive = stmt->withClause->recursive;
        qry->cteList = transformWithClause(pstate, stmt->withClause);
        qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
    }

    /* set up range table with just the result rel */
    qry->resultRelation = setTargetTable(pstate, stmt->relation,
                                         stmt->relation->inh,
                                         true,
                                         ACL_DELETE);

    /* grab the namespace item made by setTargetTable */
    nsitem = (ParseNamespaceItem *) llast(pstate->p_namespace);

    /* there's no DISTINCT in DELETE */
    qry->distinctClause = NIL;

    /* subqueries in USING cannot access the result relation */
    nsitem->p_lateral_only = true;
    nsitem->p_lateral_ok = false;

    /*
     * The USING clause is non-standard SQL syntax, and is equivalent in
     * functionality to the FROM list that can be specified for UPDATE. The
     * USING keyword is used rather than FROM because FROM is already a
     * keyword in the DELETE syntax.
     */
    transformFromClause(pstate, stmt->usingClause);

    /* remaining clauses can reference the result relation normally */
    nsitem->p_lateral_only = false;
    nsitem->p_lateral_ok = true;

    qual = transformWhereClause(pstate, stmt->whereClause,
                                EXPR_KIND_WHERE, "WHERE");

    qry->returningList = transformReturningList(pstate, stmt->returningList);

    /* done building the range table and jointree */
    qry->rtable = pstate->p_rtable;
    qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

    qry->hasSubLinks = pstate->p_hasSubLinks;
    qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
    qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
    qry->hasAggs = pstate->p_hasAggs;
    if (pstate->p_hasAggs)
        parseCheckAggregates(pstate, qry);

    assign_query_collations(pstate, qry);

    return qry;
}

/*
 * Determine whether tables of different groups are allowed to insert.
 */
static bool
is_table_allowed_insert(RelationLocInfo *from, RelationLocInfo *to)
{
	List *from_nodelist = from->rl_nodeList;
	List *to_nodelist = to->rl_nodeList;
	List *diff = NULL;
	bool result = false;

	/* necessary check, will never happened. */
	if (from == NULL || to == NULL)
	{
		elog(ERROR, "is_reptable_allow_insert, invalid params %s:%s",
			from ? " " : "from is null",
			to ? " " : "to is null");
	}

	/* step1: From table must be replication table. */
	if (
#ifdef __COLD_HOT__
		(from->coldGroupId != to->coldGroupId) ||
#endif
		((from->groupId != to->groupId) && (!IsRelationReplicated(from))))
	{
		return false;
	}

	/* step2: Data distribution nodes have intersections */
	diff = list_difference_int(to_nodelist, from_nodelist);

	/* stemp3: Insertions are allowed if there is an intersection of data distribution nodes. */
	result = (list_length(diff) != list_length(to_nodelist));
	list_free(diff);
	return result;
}

/*
 * transformInsertStmt -
 *      transform an Insert Statement
 */
static Query *
transformInsertStmt(ParseState *pstate, InsertStmt *stmt)
{// #lizard forgives
    Query       *qry = makeNode(Query);
    SelectStmt *selectStmt = (SelectStmt *) stmt->selectStmt;
    List       *exprList = NIL;
    bool        isGeneralSelect;
    List       *sub_rtable;
    List       *sub_namespace;
    List       *icolumns;
    List       *attrnos;
    RangeTblEntry *rte;
    RangeTblRef *rtr;
    ListCell   *icols;
    ListCell   *attnos;
    ListCell   *lc;
    bool        isOnConflictUpdate;
    AclMode        targetPerms;
#ifdef __TBASE__
    int ncolumns = 0;
#endif

    /* There can't be any outer WITH to worry about */
    Assert(pstate->p_ctenamespace == NIL);

    qry->commandType = CMD_INSERT;
    pstate->p_is_insert = true;
#ifdef __TBASE__
    qry->isSingleValues = false;
    qry->isMultiValues = false;
    stmt->ninsert_columns = 0;
#endif

    /* process the WITH clause independently of all else */
    if (stmt->withClause)
    {
        qry->hasRecursive = stmt->withClause->recursive;
        qry->cteList = transformWithClause(pstate, stmt->withClause);
        qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
    }

    qry->override = stmt->override;

    isOnConflictUpdate = (stmt->onConflictClause &&
                          stmt->onConflictClause->action == ONCONFLICT_UPDATE);

    /*
     * We have three cases to deal with: DEFAULT VALUES (selectStmt == NULL),
     * VALUES list, or general SELECT input.  We special-case VALUES, both for
     * efficiency and so we can handle DEFAULT specifications.
     *
     * The grammar allows attaching ORDER BY, LIMIT, FOR UPDATE, or WITH to a
     * VALUES clause.  If we have any of those, treat it as a general SELECT;
     * so it will work, but you can't use DEFAULT items together with those.
     */
    isGeneralSelect = (selectStmt && (selectStmt->valuesLists == NIL ||
                                      selectStmt->sortClause != NIL ||
                                      selectStmt->limitOffset != NULL ||
                                      selectStmt->limitCount != NULL ||
                                      selectStmt->lockingClause != NIL ||
                                      selectStmt->withClause != NULL));

    /*
     * If a non-nil rangetable/namespace was passed in, and we are doing
     * INSERT/SELECT, arrange to pass the rangetable/namespace down to the
     * SELECT.  This can only happen if we are inside a CREATE RULE, and in
     * that case we want the rule's OLD and NEW rtable entries to appear as
     * part of the SELECT's rtable, not as outer references for it.  (Kluge!)
     * The SELECT's joinlist is not affected however.  We must do this before
     * adding the target table to the INSERT's rtable.
     */
    if (isGeneralSelect)
    {
        sub_rtable = pstate->p_rtable;
        pstate->p_rtable = NIL;
        sub_namespace = pstate->p_namespace;
        pstate->p_namespace = NIL;
    }
    else
    {
        sub_rtable = NIL;        /* not used, but keep compiler quiet */
        sub_namespace = NIL;
    }

    /*
     * Must get write lock on INSERT target table before scanning SELECT, else
     * we will grab the wrong kind of initial lock if the target table is also
     * mentioned in the SELECT part.  Note that the target table is not added
     * to the joinlist or namespace.
     */
    targetPerms = ACL_INSERT;
    if (isOnConflictUpdate)
        targetPerms |= ACL_UPDATE;
    qry->resultRelation = setTargetTable(pstate, stmt->relation,
                                         false, false, targetPerms);

    /* Validate stmt->cols list, or build default list if no list given */
    icolumns = checkInsertTargets(pstate, stmt->cols, &attrnos);
    Assert(list_length(icolumns) == list_length(attrnos));

    /*
     * Determine which variant of INSERT we have.
     */
    if (selectStmt == NULL)
    {
        /*
         * We have INSERT ... DEFAULT VALUES.  We can handle this case by
         * emitting an empty targetlist --- all columns will be defaulted when
         * the planner expands the targetlist.
         */
        exprList = NIL;
    }
    else if (isGeneralSelect)
    {
        /*
         * We make the sub-pstate a child of the outer pstate so that it can
         * see any Param definitions supplied from above.  Since the outer
         * pstate's rtable and namespace are presently empty, there are no
         * side-effects of exposing names the sub-SELECT shouldn't be able to
         * see.
         */
        ParseState *sub_pstate = make_parsestate(pstate);
        Query       *selectQuery;

#ifdef __TBASE__
		/* prevent insert into cold_hot table select ... */
		if (pstate->p_target_relation)
		{
			RelationLocInfo *target_rel_loc_info = pstate->p_target_relation->rd_locator_info;
			RelationLocInfo *from_rel_loc_info;

			if (target_rel_loc_info && target_rel_loc_info->locatorType == LOCATOR_TYPE_SHARD)
			{
				foreach(lc, selectStmt->fromClause)
				{
					Node   *node = lfirst(lc);
					if (IsA(node, RangeVar))
					{
						Oid relid = RangeVarGetRelid((RangeVar *) node, NoLock, true);
						
						if (InvalidOid != relid)
						{
							Relation rel = heap_open(relid, AccessShareLock);

							from_rel_loc_info = rel->rd_locator_info;
							if (!is_table_allowed_insert(from_rel_loc_info, target_rel_loc_info))
							{
								elog(ERROR,
									"shard table could not be inserted from any other tables in different group");
							}
							
							heap_close(rel, AccessShareLock);
						}
					}
				}
			}
		}
#endif

        /*
         * Process the source SELECT.
         *
         * It is important that this be handled just like a standalone SELECT;
         * otherwise the behavior of SELECT within INSERT might be different
         * from a stand-alone SELECT. (Indeed, Postgres up through 6.5 had
         * bugs of just that nature...)
         *
         * The sole exception is that we prevent resolving unknown-type
         * outputs as TEXT.  This does not change the semantics since if the
         * column type matters semantically, it would have been resolved to
         * something else anyway.  Doing this lets us resolve such outputs as
         * the target column's type, which we handle below.
         */
        sub_pstate->p_rtable = sub_rtable;
        sub_pstate->p_joinexprs = NIL;    /* sub_rtable has no joins */
        sub_pstate->p_namespace = sub_namespace;
        sub_pstate->p_resolve_unknowns = false;

        selectQuery = transformStmt(sub_pstate, stmt->selectStmt);

        free_parsestate(sub_pstate);

        /* The grammar should have produced a SELECT */
        if (!IsA(selectQuery, Query) ||
            selectQuery->commandType != CMD_SELECT)
            elog(ERROR, "unexpected non-SELECT command in INSERT ... SELECT");

        /*
         * Make the source be a subquery in the INSERT's rangetable, and add
         * it to the INSERT's joinlist.
         */
        rte = addRangeTableEntryForSubquery(pstate,
                                            selectQuery,
                                            makeAlias("*SELECT*", NIL),
                                            false,
                                            false);
        rtr = makeNode(RangeTblRef);
        /* assume new rte is at end */
        rtr->rtindex = list_length(pstate->p_rtable);
        Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
        pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);

        /*----------
         * Generate an expression list for the INSERT that selects all the
         * non-resjunk columns from the subquery.  (INSERT's tlist must be
         * separate from the subquery's tlist because we may add columns,
         * insert datatype coercions, etc.)
         *
         * HACK: unknown-type constants and params in the SELECT's targetlist
         * are copied up as-is rather than being referenced as subquery
         * outputs.  This is to ensure that when we try to coerce them to
         * the target column's datatype, the right things happen (see
         * special cases in coerce_type).  Otherwise, this fails:
         *        INSERT INTO foo SELECT 'bar', ... FROM baz
         *----------
         */
        exprList = NIL;
        foreach(lc, selectQuery->targetList)
        {
            TargetEntry *tle = (TargetEntry *) lfirst(lc);
            Expr       *expr;

            if (tle->resjunk)
                continue;
            if (tle->expr &&
                (IsA(tle->expr, Const) ||IsA(tle->expr, Param)) &&
                exprType((Node *) tle->expr) == UNKNOWNOID)
                expr = tle->expr;
            else
            {
                Var           *var = makeVarFromTargetEntry(rtr->rtindex, tle);

                var->location = exprLocation((Node *) tle->expr);
                expr = (Expr *) var;
            }
            exprList = lappend(exprList, expr);
        }

        /* Prepare row for assignment to target table */
        exprList = transformInsertRow(pstate, exprList,
                                      stmt->cols,
                                      icolumns, attrnos,
                                      false);
    }
    else if (list_length(selectStmt->valuesLists) > 1)
    {
        /*
         * Process INSERT ... VALUES with multiple VALUES sublists. We
         * generate a VALUES RTE holding the transformed expression lists, and
         * build up a targetlist containing Vars that reference the VALUES
         * RTE.
         */
        List       *exprsLists = NIL;
        List       *coltypes = NIL;
        List       *coltypmods = NIL;
        List       *colcollations = NIL;
        int            sublist_length = -1;
        bool        lateral = false;

        Assert(selectStmt->intoClause == NULL);

#ifdef __COLD_HOT__
        /* prevent insert into cold_hot table select ... */
        if (pstate->p_target_relation)
        {
            RelationLocInfo *rel_loc_info = pstate->p_target_relation->rd_locator_info;

            if (rel_loc_info)
            {
                if (AttributeNumberIsValid(rel_loc_info->secAttrNum) 
                    || OidIsValid(rel_loc_info->coldGroupId))
                {
                    elog(ERROR, "table in cold-hot group or key-value group could not join with other tables.");
                }
            }
        }
#endif

#ifdef __TBASE__
        /*
         * transform 'insert into values' into 'COPY FROM', only handle
         * distributed relation(by hash/shard/replication) without
         * on conflict/returning/with clause/triggers.
         */
        if (g_transform_insert_to_copy && IS_PGXC_COORDINATOR &&
            !stmt->onConflictClause && !stmt->returningList &&
            !stmt->withClause && !explain_stmt)
        {
            if (pstate->p_target_relation)
            {
                RelationLocInfo *rel_loc_info = pstate->p_target_relation->rd_locator_info;

                if (rel_loc_info->locatorType == LOCATOR_TYPE_HASH ||
                    rel_loc_info->locatorType == LOCATOR_TYPE_SHARD ||
                    rel_loc_info->locatorType == LOCATOR_TYPE_REPLICATED)
                {
                    qry->isMultiValues = true;

                    /* has triggers? */
                    if (!pgxc_check_triggers_shippability(RelationGetRelid(pstate->p_target_relation),
                                                      qry->commandType))
                    {
                        qry->hasUnshippableTriggers = true;
                    }
                }
            }
        }

        /*
         * put values into memory, then copy to remote datanode
         */
        if (qry->isMultiValues && !qry->hasUnshippableTriggers)
        {
            int            i;
            bool  copy_from = true;
            int   ndatarows = 0;
            int   column_index = 0;
            char  ***data_list = NULL;

            ncolumns = 0;
            
            if (stmt->cols)
            {
                ncolumns = list_length(stmt->cols);
            }
            else
            {
                /* Generate default column list */
                TupleDesc tupDesc = RelationGetDescr(pstate->p_target_relation);
                Form_pg_attribute *attr = tupDesc->attrs;
                int            attr_count = tupDesc->natts;

                for (i = 0; i < attr_count; i++)
                {
                    if (attr[i]->attisdropped)
                        continue;
                    ncolumns++;
                }
            }

            stmt->ninsert_columns = ncolumns;
            
            /* 
             * if in extend PROTOCOL, let bind handle it 
             */
            if (IsExtendedQuery())
            {
                goto TRANSFORM_VALUELISTS;
            }

            exprList = NULL;

            /* transform all values into memory */
            ndatarows = list_length(selectStmt->valuesLists);

            data_list = (char ***)palloc0(sizeof(char **) * ndatarows);
            for (i = 0; i < ndatarows; i++)
            {
                data_list[i] = (char **)palloc0(sizeof(char *) * ncolumns);
            }
            
            column_index = 0;
            foreach(lc, selectStmt->valuesLists)
            {
                int index;
                int count;
                ListCell *cell;
                List       *sublist = (List *) lfirst(lc);
                
                index = 0;
                /*
                 * number of values does not match insert columns, unexpected case,
                 * do not copy from
                 */
                count = list_length(sublist);
                if (ncolumns != count)
                {
                    copy_from = false;
                    break;
                }
                foreach(cell, sublist)
                {
                    A_Const *v = (A_Const *)lfirst(cell);

                    /* we can just handle simple case, the value must be const */
                    switch(nodeTag(v))
                    {
                        case T_A_Const:
                            break;
                        case T_TypeCast:
                        {
                            TypeCast *cast = (TypeCast *)v;

                            if (IsA(cast->arg, A_Const))
                            {
                                v = (A_Const *)cast->arg;
                            }
                            else
                            {
                                copy_from = false;
                            }
                            break;
                        }
                        default:
                            {
                                copy_from = false;
                                break;
                            }
                    }

                    /* COPY_FROM could not read from file */
                    if (!copy_from)
                    {
                        break;
                    }

                    index++;

                    /* A_Const */
                    switch(v->val.type)
                    {
                        case T_Integer:
                            {
                                StringInfoData data;
                                initStringInfo(&data);
                                appendStringInfo(&data, "%ld", v->val.val.ival);
                                data_list[column_index][index - 1] = data.data;
                                if (index >= count)
                                {
                                    column_index++;
                                }
                                break;
                            }
                        case T_Float:
                        case T_String:
                        case T_BitString:
                            {
                                data_list[column_index][index - 1] = pstrdup(v->val.val.str);
                                if (index >= count)
                                {
                                    column_index++;
                                }
                                break;
                            }
                        case T_Null:
                            {
                                data_list[column_index][index - 1] = NULL;
                                if (index >= count)
                                {
                                    column_index++;
                                }
                                break;
                            }
                        default:
                            elog(ERROR, "unknown value type %d", v->val.type);
                    }
                }

                if (!copy_from)
                {
                    break;
                }
                else
                {
                    /* sanity check */
                    if (index != count)
                    {
                        elog(ERROR, "insert columns mismatched, expected %d, result %d",
                                     count, index);
                    }
                }
            }

            if (copy_from)
            {
                if (ndatarows != column_index)
                {
                    elog(ERROR, "datarow count mismatched, expected %d, result %d",
                                 ndatarows, column_index);
                }
                qry->copy_filename = palloc(MAXPGPATH);
                snprintf(qry->copy_filename, MAXPGPATH, "%s", "Insert_into to Copy_from(Simple Protocl)");
                stmt->ndatarows = ndatarows;
                stmt->data_list = data_list;
            }
            else
            {
                qry->isMultiValues = false;
                goto TRANSFORM_VALUELISTS;
            }
        }
        else
        {
#endif
TRANSFORM_VALUELISTS:
        foreach(lc, selectStmt->valuesLists)
        {
            List       *sublist = (List *) lfirst(lc);

            /*
             * Do basic expression transformation (same as a ROW() expr, but
             * allow SetToDefault at top level)
             */
            sublist = transformExpressionList(pstate, sublist,
                                              EXPR_KIND_VALUES, true);

            /*
             * All the sublists must be the same length, *after*
             * transformation (which might expand '*' into multiple items).
             * The VALUES RTE can't handle anything different.
             */
            if (sublist_length < 0)
            {
                /* Remember post-transformation length of first sublist */
                sublist_length = list_length(sublist);
            }
            else if (sublist_length != list_length(sublist))
            {
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("VALUES lists must all be the same length"),
                         parser_errposition(pstate,
                                            exprLocation((Node *) sublist))));
            }

#ifdef __TBASE__
            if (IsExtendedQuery() && qry->isMultiValues && !qry->hasUnshippableTriggers)
            {
                /*
                 * simple insert if all values are params
                 *
                 * if not simple insert, do not transform insert into to copy from
                 */
                ListCell *cell;
                foreach(cell, sublist)
                {
                    Node *node = (Node *)lfirst(cell);
                    if (!IsA(node, Param))
                    {
                        qry->isMultiValues = false;
                        break;
                    }
                }
            }
#endif

            /*
             * Prepare row for assignment to target table.  We process any
             * indirection on the target column specs normally but then strip
             * off the resulting field/array assignment nodes, since we don't
             * want the parsed statement to contain copies of those in each
             * VALUES row.  (It's annoying to have to transform the
             * indirection specs over and over like this, but avoiding it
             * would take some really messy refactoring of
             * transformAssignmentIndirection.)
             */
            sublist = transformInsertRow(pstate, sublist,
                                         stmt->cols,
                                         icolumns, attrnos,
                                         true);

            /*
             * We must assign collations now because assign_query_collations
             * doesn't process rangetable entries.  We just assign all the
             * collations independently in each row, and don't worry about
             * whether they are consistent vertically.  The outer INSERT query
             * isn't going to care about the collations of the VALUES columns,
             * so it's not worth the effort to identify a common collation for
             * each one here.  (But note this does have one user-visible
             * consequence: INSERT ... VALUES won't complain about conflicting
             * explicit COLLATEs in a column, whereas the same VALUES
             * construct in another context would complain.)
             */
            assign_list_collations(pstate, sublist);

            exprsLists = lappend(exprsLists, sublist);
        }

#ifdef __TBASE__
        /* number of insert columns must be same as valueslist */
        if (IsExtendedQuery() && qry->isMultiValues && !qry->hasUnshippableTriggers)
        {
            if (ncolumns != sublist_length)
            {
                qry->isMultiValues = false;
            }
        }
#endif

        /*
         * Construct column type/typmod/collation lists for the VALUES RTE.
         * Every expression in each column has been coerced to the type/typmod
         * of the corresponding target column or subfield, so it's sufficient
         * to look at the exprType/exprTypmod of the first row.  We don't care
         * about the collation labeling, so just fill in InvalidOid for that.
         */
        foreach(lc, (List *) linitial(exprsLists))
        {
            Node       *val = (Node *) lfirst(lc);

            coltypes = lappend_oid(coltypes, exprType(val));
            coltypmods = lappend_int(coltypmods, exprTypmod(val));
            colcollations = lappend_oid(colcollations, InvalidOid);
        }

        /*
         * Ordinarily there can't be any current-level Vars in the expression
         * lists, because the namespace was empty ... but if we're inside
         * CREATE RULE, then NEW/OLD references might appear.  In that case we
         * have to mark the VALUES RTE as LATERAL.
         */
        if (list_length(pstate->p_rtable) != 1 &&
            contain_vars_of_level((Node *) exprsLists, 0))
            lateral = true;

        /*
         * Generate the VALUES RTE
         */
        rte = addRangeTableEntryForValues(pstate, exprsLists,
                                          coltypes, coltypmods, colcollations,
                                          NULL, lateral, true);
        rtr = makeNode(RangeTblRef);
        /* assume new rte is at end */
        rtr->rtindex = list_length(pstate->p_rtable);
        Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
        pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);

        /*
         * Generate list of Vars referencing the RTE
         */
        expandRTE(rte, rtr->rtindex, 0, -1, false, NULL, &exprList);

        /*
         * Re-apply any indirection on the target column specs to the Vars
         */
        exprList = transformInsertRow(pstate, exprList,
                                      stmt->cols,
                                      icolumns, attrnos,
                                      false);
#ifdef __TBASE__
        }
#endif
    }
    else
    {
        /*
         * Process INSERT ... VALUES with a single VALUES sublist.  We treat
         * this case separately for efficiency.  The sublist is just computed
         * directly as the Query's targetlist, with no VALUES RTE.  So it
         * works just like a SELECT without any FROM.
         */
        List       *valuesLists = selectStmt->valuesLists;

        Assert(list_length(valuesLists) == 1);
        Assert(selectStmt->intoClause == NULL);

        /*
         * Do basic expression transformation (same as a ROW() expr, but allow
         * SetToDefault at top level)
         */
        exprList = transformExpressionList(pstate,
                                           (List *) linitial(valuesLists),
                                           EXPR_KIND_VALUES_SINGLE,
                                           true);

        /* Prepare row for assignment to target table */
        exprList = transformInsertRow(pstate, exprList,
                                      stmt->cols,
                                      icolumns, attrnos,
                                      false);
#ifdef __TBASE__
        if(RELATION_IS_INTERVAL(pstate->p_target_relation))
        {
            int partcol = InvalidAttrNumber;
            int att_no;
            
            ListCell    *cl1, *cl2;
            Node        *expr;
            bool        allareconst = true;

            partcol = RelationGetPartitionColumnIndex(pstate->p_target_relation);
            
            forboth(cl1,exprList,cl2, attrnos)
            {
                expr = (Node*)lfirst(cl1);
                att_no = lfirst_int(cl2);
                if(att_no == partcol && !IsA(expr,Const))
                {
                    allareconst = false;
                    break;
                }
            }
            qry->isSingleValues = allareconst;
        }        
#endif
    }

    /*
     * Generate query's target list using the computed list of expressions.
     * Also, mark all the target columns as needing insert permissions.
     */
    rte = pstate->p_target_rangetblentry;
    qry->targetList = NIL;
    icols = list_head(icolumns);
    attnos = list_head(attrnos);
    foreach(lc, exprList)
    {
        Expr       *expr = (Expr *) lfirst(lc);
        ResTarget  *col;
        AttrNumber    attr_num;
        TargetEntry *tle;

        col = lfirst_node(ResTarget, icols);
        attr_num = (AttrNumber) lfirst_int(attnos);

        tle = makeTargetEntry(expr,
                              attr_num,
                              col->name,
                              false);
        qry->targetList = lappend(qry->targetList, tle);

        rte->insertedCols = bms_add_member(rte->insertedCols,
                                           attr_num - FirstLowInvalidHeapAttributeNumber);

        icols = lnext(icols);
        attnos = lnext(attnos);
    }

    /* Process ON CONFLICT, if any. */
    if (stmt->onConflictClause)
        qry->onConflict = transformOnConflictClause(pstate,
                                                    stmt->onConflictClause);

    /*
     * If we have a RETURNING clause, we need to add the target relation to
     * the query namespace before processing it, so that Var references in
     * RETURNING will work.  Also, remove any namespace entries added in a
     * sub-SELECT or VALUES list.
     */
    if (stmt->returningList)
    {
        pstate->p_namespace = NIL;
        addRTEtoQuery(pstate, pstate->p_target_rangetblentry,
                      false, true, true);
        qry->returningList = transformReturningList(pstate,
                                                    stmt->returningList);
    }

    /* done building the range table and jointree */
    qry->rtable = pstate->p_rtable;
    qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
    qry->hasSubLinks = pstate->p_hasSubLinks;

    assign_query_collations(pstate, qry);

    return qry;
}

/*
 * Prepare an INSERT row for assignment to the target table.
 *
 * exprlist: transformed expressions for source values; these might come from
 * a VALUES row, or be Vars referencing a sub-SELECT or VALUES RTE output.
 * stmtcols: original target-columns spec for INSERT (we just test for NIL)
 * icolumns: effective target-columns spec (list of ResTarget)
 * attrnos: integer column numbers (must be same length as icolumns)
 * strip_indirection: if true, remove any field/array assignment nodes
 */
static List *
transformInsertRow(ParseState *pstate, List *exprlist,
                   List *stmtcols, List *icolumns, List *attrnos,
                   bool strip_indirection)
{// #lizard forgives
    List       *result;
    ListCell   *lc;
    ListCell   *icols;
    ListCell   *attnos;

    /*
     * Check length of expr list.  It must not have more expressions than
     * there are target columns.  We allow fewer, but only if no explicit
     * columns list was given (the remaining columns are implicitly
     * defaulted).  Note we must check this *after* transformation because
     * that could expand '*' into multiple items.
     */
    if (list_length(exprlist) > list_length(icolumns))
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("INSERT has more expressions than target columns"),
                 parser_errposition(pstate,
                                    exprLocation(list_nth(exprlist,
                                                          list_length(icolumns))))));
    if (stmtcols != NIL &&
        list_length(exprlist) < list_length(icolumns))
    {
        /*
         * We can get here for cases like INSERT ... SELECT (a,b,c) FROM ...
         * where the user accidentally created a RowExpr instead of separate
         * columns.  Add a suitable hint if that seems to be the problem,
         * because the main error message is quite misleading for this case.
         * (If there's no stmtcols, you'll get something about data type
         * mismatch, which is less misleading so we don't worry about giving a
         * hint in that case.)
         */
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("INSERT has more target columns than expressions"),
                 ((list_length(exprlist) == 1 &&
                   count_rowexpr_columns(pstate, linitial(exprlist)) ==
                   list_length(icolumns)) ?
                  errhint("The insertion source is a row expression containing the same number of columns expected by the INSERT. Did you accidentally use extra parentheses?") : 0),
                 parser_errposition(pstate,
                                    exprLocation(list_nth(icolumns,
                                                          list_length(exprlist))))));
    }

    /*
     * Prepare columns for assignment to target table.
     */
    result = NIL;
    icols = list_head(icolumns);
    attnos = list_head(attrnos);
    foreach(lc, exprlist)
    {
        Expr       *expr = (Expr *) lfirst(lc);
        ResTarget  *col;

        col = lfirst_node(ResTarget, icols);

        expr = transformAssignedExpr(pstate, expr,
                                     EXPR_KIND_INSERT_TARGET,
                                     col->name,
                                     lfirst_int(attnos),
                                     col->indirection,
                                     col->location);

        if (strip_indirection)
        {
            while (expr)
            {
                if (IsA(expr, FieldStore))
                {
                    FieldStore *fstore = (FieldStore *) expr;

                    expr = (Expr *) linitial(fstore->newvals);
                }
                else if (IsA(expr, ArrayRef))
                {
                    ArrayRef   *aref = (ArrayRef *) expr;

                    if (aref->refassgnexpr == NULL)
                        break;
                    expr = aref->refassgnexpr;
                }
                else
                    break;
            }
        }

        result = lappend(result, expr);

        icols = lnext(icols);
        attnos = lnext(attnos);
    }

    return result;
}

/*
 * transformOnConflictClause -
 *      transforms an OnConflictClause in an INSERT
 */
static OnConflictExpr *
transformOnConflictClause(ParseState *pstate,
                          OnConflictClause *onConflictClause)
{
    List       *arbiterElems;
    Node       *arbiterWhere;
    Oid            arbiterConstraint;
    List       *onConflictSet = NIL;
    Node       *onConflictWhere = NULL;
    RangeTblEntry *exclRte = NULL;
    int            exclRelIndex = 0;
    List       *exclRelTlist = NIL;
    OnConflictExpr *result;

    /* Process the arbiter clause, ON CONFLICT ON (...) */
    transformOnConflictArbiter(pstate, onConflictClause, &arbiterElems,
                               &arbiterWhere, &arbiterConstraint);

    /* Process DO UPDATE */
    if (onConflictClause->action == ONCONFLICT_UPDATE)
    {
        Relation    targetrel = pstate->p_target_relation;
        Var           *var;
        TargetEntry *te;
        int            attno;

		if (targetrel->rd_partdesc)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("%s cannot be applied to partitioned table \"%s\"",
							"ON CONFLICT DO UPDATE",
							RelationGetRelationName(targetrel))));

        /*
         * All INSERT expressions have been parsed, get ready for potentially
         * existing SET statements that need to be processed like an UPDATE.
         */
        pstate->p_is_insert = false;

        /*
         * Add range table entry for the EXCLUDED pseudo relation; relkind is
         * set to composite to signal that we're not dealing with an actual
         * relation.
         */
        exclRte = addRangeTableEntryForRelation(pstate,
                                                targetrel,
                                                makeAlias("excluded", NIL),
                                                false, false);
        exclRte->relkind = RELKIND_COMPOSITE_TYPE;
        exclRelIndex = list_length(pstate->p_rtable);

        /*
         * Build a targetlist representing the columns of the EXCLUDED pseudo
         * relation.  Have to be careful to use resnos that correspond to
         * attnos of the underlying relation.
         */
        for (attno = 0; attno < targetrel->rd_rel->relnatts; attno++)
        {
            Form_pg_attribute attr = targetrel->rd_att->attrs[attno];
            char       *name;

            if (attr->attisdropped)
            {
                /*
                 * can't use atttypid here, but it doesn't really matter what
                 * type the Const claims to be.
                 */
                var = (Var *) makeNullConst(INT4OID, -1, InvalidOid);
                name = "";
            }
            else
            {
                var = makeVar(exclRelIndex, attno + 1,
                              attr->atttypid, attr->atttypmod,
                              attr->attcollation,
                              0);
                name = pstrdup(NameStr(attr->attname));
            }

            te = makeTargetEntry((Expr *) var,
                                 attno + 1,
                                 name,
                                 false);

            /* don't require select access yet */
            exclRelTlist = lappend(exclRelTlist, te);
        }

        /*
         * Add a whole-row-Var entry to support references to "EXCLUDED.*".
         * Like the other entries in exclRelTlist, its resno must match the
         * Var's varattno, else the wrong things happen while resolving
         * references in setrefs.c.  This is against normal conventions for
         * targetlists, but it's okay since we don't use this as a real tlist.
         */
        var = makeVar(exclRelIndex, InvalidAttrNumber,
                      targetrel->rd_rel->reltype,
                      -1, InvalidOid, 0);
        te = makeTargetEntry((Expr *) var, InvalidAttrNumber, NULL, true);
        exclRelTlist = lappend(exclRelTlist, te);

        /*
         * Add EXCLUDED and the target RTE to the namespace, so that they can
         * be used in the UPDATE statement.
         */
        addRTEtoQuery(pstate, exclRte, false, true, true);
        addRTEtoQuery(pstate, pstate->p_target_rangetblentry,
                      false, true, true);

        onConflictSet =
            transformUpdateTargetList(pstate, onConflictClause->targetList);

        onConflictWhere = transformWhereClause(pstate,
                                               onConflictClause->whereClause,
                                               EXPR_KIND_WHERE, "WHERE");
    }

    /* Finally, build ON CONFLICT DO [NOTHING | UPDATE] expression */
    result = makeNode(OnConflictExpr);

    result->action = onConflictClause->action;
    result->arbiterElems = arbiterElems;
    result->arbiterWhere = arbiterWhere;
    result->constraint = arbiterConstraint;
    result->onConflictSet = onConflictSet;
    result->onConflictWhere = onConflictWhere;
    result->exclRelIndex = exclRelIndex;
    result->exclRelTlist = exclRelTlist;

    return result;
}


/*
 * count_rowexpr_columns -
 *      get number of columns contained in a ROW() expression;
 *      return -1 if expression isn't a RowExpr or a Var referencing one.
 *
 * This is currently used only for hint purposes, so we aren't terribly
 * tense about recognizing all possible cases.  The Var case is interesting
 * because that's what we'll get in the INSERT ... SELECT (...) case.
 */
static int
count_rowexpr_columns(ParseState *pstate, Node *expr)
{// #lizard forgives
    if (expr == NULL)
        return -1;
    if (IsA(expr, RowExpr))
        return list_length(((RowExpr *) expr)->args);
    if (IsA(expr, Var))
    {
        Var           *var = (Var *) expr;
        AttrNumber    attnum = var->varattno;

        if (attnum > 0 && var->vartype == RECORDOID)
        {
            RangeTblEntry *rte;

            rte = GetRTEByRangeTablePosn(pstate, var->varno, var->varlevelsup);
            if (rte->rtekind == RTE_SUBQUERY)
            {
                /* Subselect-in-FROM: examine sub-select's output expr */
                TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
                                                    attnum);

                if (ste == NULL || ste->resjunk)
                    return -1;
                expr = (Node *) ste->expr;
                if (IsA(expr, RowExpr))
                    return list_length(((RowExpr *) expr)->args);
            }
        }
    }
    return -1;
}


/*
 * transformSelectStmt -
 *      transforms a Select Statement
 *
 * Note: this covers only cases with no set operations and no VALUES lists;
 * see below for the other cases.
 */
static Query *
transformSelectStmt(ParseState *pstate, SelectStmt *stmt)
{// #lizard forgives
    Query       *qry = makeNode(Query);
    Node       *qual;
    ListCell   *l;

    qry->commandType = CMD_SELECT;

    /* process the WITH clause independently of all else */
    if (stmt->withClause)
    {
        qry->hasRecursive = stmt->withClause->recursive;
        qry->cteList = transformWithClause(pstate, stmt->withClause);
        qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
    }

    /* Complain if we get called from someplace where INTO is not allowed */
    if (stmt->intoClause)
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("SELECT ... INTO is not allowed here"),
                 parser_errposition(pstate,
                                    exprLocation((Node *) stmt->intoClause))));

    /* make FOR UPDATE/FOR SHARE info available to addRangeTableEntry */
    pstate->p_locking_clause = stmt->lockingClause;

    /* make WINDOW info available for window functions, too */
    pstate->p_windowdefs = stmt->windowClause;

    /* process the FROM clause */
    transformFromClause(pstate, stmt->fromClause);

    /* transform targetlist */
    qry->targetList = transformTargetList(pstate, stmt->targetList,
                                          EXPR_KIND_SELECT_TARGET);

    /* mark column origins */
    markTargetListOrigins(pstate, qry->targetList);

    /* transform WHERE */
    qual = transformWhereClause(pstate, stmt->whereClause,
                                EXPR_KIND_WHERE, "WHERE");

    /* initial processing of HAVING clause is much like WHERE clause */
    qry->havingQual = transformWhereClause(pstate, stmt->havingClause,
                                           EXPR_KIND_HAVING, "HAVING");

    /*
     * Transform sorting/grouping stuff.  Do ORDER BY first because both
     * transformGroupClause and transformDistinctClause need the results. Note
     * that these functions can also change the targetList, so it's passed to
     * them by reference.
     */
    qry->sortClause = transformSortClause(pstate,
                                          stmt->sortClause,
                                          &qry->targetList,
                                          EXPR_KIND_ORDER_BY,
                                          false /* allow SQL92 rules */ );

    qry->groupClause = transformGroupClause(pstate,
                                            stmt->groupClause,
                                            &qry->groupingSets,
                                            &qry->targetList,
                                            qry->sortClause,
                                            EXPR_KIND_GROUP_BY,
                                            false /* allow SQL92 rules */ );

    if (stmt->distinctClause == NIL)
    {
        qry->distinctClause = NIL;
        qry->hasDistinctOn = false;
    }
    else if (linitial(stmt->distinctClause) == NULL)
    {
        /* We had SELECT DISTINCT */
        qry->distinctClause = transformDistinctClause(pstate,
                                                      &qry->targetList,
                                                      qry->sortClause,
                                                      false);
        qry->hasDistinctOn = false;
    }
    else
    {
        /* We had SELECT DISTINCT ON */
        qry->distinctClause = transformDistinctOnClause(pstate,
                                                        stmt->distinctClause,
                                                        &qry->targetList,
                                                        qry->sortClause);
        qry->hasDistinctOn = true;
    }

    /* transform LIMIT */
    qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
                                            EXPR_KIND_OFFSET, "OFFSET");
    qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
                                           EXPR_KIND_LIMIT, "LIMIT");

    /* transform window clauses after we have seen all window functions */
    qry->windowClause = transformWindowDefinitions(pstate,
                                                   pstate->p_windowdefs,
                                                   &qry->targetList);

    /* resolve any still-unresolved output columns as being type text */
    if (pstate->p_resolve_unknowns)
        resolveTargetListUnknowns(pstate, qry->targetList);

    qry->rtable = pstate->p_rtable;
    qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

    qry->hasSubLinks = pstate->p_hasSubLinks;
    qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
    qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
    qry->hasAggs = pstate->p_hasAggs;
    if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
        parseCheckAggregates(pstate, qry);

    foreach(l, stmt->lockingClause)
    {
        transformLockingClause(pstate, qry,
                               (LockingClause *) lfirst(l), false);
    }

    assign_query_collations(pstate, qry);

    return qry;
}

/*
 * transformValuesClause -
 *      transforms a VALUES clause that's being used as a standalone SELECT
 *
 * We build a Query containing a VALUES RTE, rather as if one had written
 *            SELECT * FROM (VALUES ...) AS "*VALUES*"
 */
static Query *
transformValuesClause(ParseState *pstate, SelectStmt *stmt)
{// #lizard forgives
    Query       *qry = makeNode(Query);
    List       *exprsLists;
    List       *coltypes = NIL;
    List       *coltypmods = NIL;
    List       *colcollations = NIL;
    List      **colexprs = NULL;
    int            sublist_length = -1;
    bool        lateral = false;
    RangeTblEntry *rte;
    int            rtindex;
    ListCell   *lc;
    ListCell   *lc2;
    int            i;

    qry->commandType = CMD_SELECT;

    /* Most SELECT stuff doesn't apply in a VALUES clause */
    Assert(stmt->distinctClause == NIL);
    Assert(stmt->intoClause == NULL);
    Assert(stmt->targetList == NIL);
    Assert(stmt->fromClause == NIL);
    Assert(stmt->whereClause == NULL);
    Assert(stmt->groupClause == NIL);
    Assert(stmt->havingClause == NULL);
    Assert(stmt->windowClause == NIL);
    Assert(stmt->op == SETOP_NONE);

    /* process the WITH clause independently of all else */
    if (stmt->withClause)
    {
        qry->hasRecursive = stmt->withClause->recursive;
        qry->cteList = transformWithClause(pstate, stmt->withClause);
        qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
    }

    /*
     * For each row of VALUES, transform the raw expressions.
     *
     * Note that the intermediate representation we build is column-organized
     * not row-organized.  That simplifies the type and collation processing
     * below.
     */
    foreach(lc, stmt->valuesLists)
    {
        List       *sublist = (List *) lfirst(lc);

        /*
         * Do basic expression transformation (same as a ROW() expr, but here
         * we disallow SetToDefault)
         */
        sublist = transformExpressionList(pstate, sublist,
                                          EXPR_KIND_VALUES, false);

        /*
         * All the sublists must be the same length, *after* transformation
         * (which might expand '*' into multiple items).  The VALUES RTE can't
         * handle anything different.
         */
        if (sublist_length < 0)
        {
            /* Remember post-transformation length of first sublist */
            sublist_length = list_length(sublist);
            /* and allocate array for per-column lists */
            colexprs = (List **) palloc0(sublist_length * sizeof(List *));
        }
        else if (sublist_length != list_length(sublist))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                     errmsg("VALUES lists must all be the same length"),
                     parser_errposition(pstate,
                                        exprLocation((Node *) sublist))));
        }

        /* Build per-column expression lists */
        i = 0;
        foreach(lc2, sublist)
        {
            Node       *col = (Node *) lfirst(lc2);

            colexprs[i] = lappend(colexprs[i], col);
            i++;
        }

        /* Release sub-list's cells to save memory */
        list_free(sublist);
    }

    /*
     * Now resolve the common types of the columns, and coerce everything to
     * those types.  Then identify the common typmod and common collation, if
     * any, of each column.
     *
     * We must do collation processing now because (1) assign_query_collations
     * doesn't process rangetable entries, and (2) we need to label the VALUES
     * RTE with column collations for use in the outer query.  We don't
     * consider conflict of implicit collations to be an error here; instead
     * the column will just show InvalidOid as its collation, and you'll get a
     * failure later if that results in failure to resolve a collation.
     *
     * Note we modify the per-column expression lists in-place.
     */
    for (i = 0; i < sublist_length; i++)
    {
        Oid            coltype;
        int32        coltypmod = -1;
        Oid            colcoll;
        bool        first = true;

        coltype = select_common_type(pstate, colexprs[i], "VALUES", NULL);

        foreach(lc, colexprs[i])
        {
            Node       *col = (Node *) lfirst(lc);

            col = coerce_to_common_type(pstate, col, coltype, "VALUES");
            lfirst(lc) = (void *) col;
            if (first)
            {
                coltypmod = exprTypmod(col);
                first = false;
            }
            else
            {
                /* As soon as we see a non-matching typmod, fall back to -1 */
                if (coltypmod >= 0 && coltypmod != exprTypmod(col))
                    coltypmod = -1;
            }
        }

        colcoll = select_common_collation(pstate, colexprs[i], true);

        coltypes = lappend_oid(coltypes, coltype);
        coltypmods = lappend_int(coltypmods, coltypmod);
        colcollations = lappend_oid(colcollations, colcoll);
    }

    /*
     * Finally, rearrange the coerced expressions into row-organized lists.
     */
    exprsLists = NIL;
    foreach(lc, colexprs[0])
    {
        Node       *col = (Node *) lfirst(lc);
        List       *sublist;

        sublist = list_make1(col);
        exprsLists = lappend(exprsLists, sublist);
    }
    list_free(colexprs[0]);
    for (i = 1; i < sublist_length; i++)
    {
        forboth(lc, colexprs[i], lc2, exprsLists)
        {
            Node       *col = (Node *) lfirst(lc);
            List       *sublist = lfirst(lc2);

            /* sublist pointer in exprsLists won't need adjustment */
            (void) lappend(sublist, col);
        }
        list_free(colexprs[i]);
    }

    /*
     * Ordinarily there can't be any current-level Vars in the expression
     * lists, because the namespace was empty ... but if we're inside CREATE
     * RULE, then NEW/OLD references might appear.  In that case we have to
     * mark the VALUES RTE as LATERAL.
     */
    if (pstate->p_rtable != NIL &&
        contain_vars_of_level((Node *) exprsLists, 0))
        lateral = true;

    /*
     * Generate the VALUES RTE
     */
    rte = addRangeTableEntryForValues(pstate, exprsLists,
                                      coltypes, coltypmods, colcollations,
                                      NULL, lateral, true);
    addRTEtoQuery(pstate, rte, true, true, true);

    /* assume new rte is at end */
    rtindex = list_length(pstate->p_rtable);
    Assert(rte == rt_fetch(rtindex, pstate->p_rtable));

    /*
     * Generate a targetlist as though expanding "*"
     */
    Assert(pstate->p_next_resno == 1);
    qry->targetList = expandRelAttrs(pstate, rte, rtindex, 0, -1);

    /*
     * The grammar allows attaching ORDER BY, LIMIT, and FOR UPDATE to a
     * VALUES, so cope.
     */
    qry->sortClause = transformSortClause(pstate,
                                          stmt->sortClause,
                                          &qry->targetList,
                                          EXPR_KIND_ORDER_BY,
                                          false /* allow SQL92 rules */ );

    qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
                                            EXPR_KIND_OFFSET, "OFFSET");
    qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
                                           EXPR_KIND_LIMIT, "LIMIT");

    if (stmt->lockingClause)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s cannot be applied to VALUES",
                        LCS_asString(((LockingClause *)
                                      linitial(stmt->lockingClause))->strength))));

    qry->rtable = pstate->p_rtable;
    qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    qry->hasSubLinks = pstate->p_hasSubLinks;

    assign_query_collations(pstate, qry);

    return qry;
}

/*
 * transformSetOperationStmt -
 *      transforms a set-operations tree
 *
 * A set-operation tree is just a SELECT, but with UNION/INTERSECT/EXCEPT
 * structure to it.  We must transform each leaf SELECT and build up a top-
 * level Query that contains the leaf SELECTs as subqueries in its rangetable.
 * The tree of set operations is converted into the setOperations field of
 * the top-level Query.
 */
static Query *
transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt)
{// #lizard forgives
    Query       *qry = makeNode(Query);
    SelectStmt *leftmostSelect;
    int            leftmostRTI;
    Query       *leftmostQuery;
    SetOperationStmt *sostmt;
    List       *sortClause;
    Node       *limitOffset;
    Node       *limitCount;
    List       *lockingClause;
    WithClause *withClause;
    Node       *node;
    ListCell   *left_tlist,
               *lct,
               *lcm,
               *lcc,
               *l;
    List       *targetvars,
               *targetnames,
               *sv_namespace;
    int            sv_rtable_length;
    RangeTblEntry *jrte;
    int            tllen;

    qry->commandType = CMD_SELECT;

    /*
     * Find leftmost leaf SelectStmt.  We currently only need to do this in
     * order to deliver a suitable error message if there's an INTO clause
     * there, implying the set-op tree is in a context that doesn't allow
     * INTO.  (transformSetOperationTree would throw error anyway, but it
     * seems worth the trouble to throw a different error for non-leftmost
     * INTO, so we produce that error in transformSetOperationTree.)
     */
    leftmostSelect = stmt->larg;
    while (leftmostSelect && leftmostSelect->op != SETOP_NONE)
        leftmostSelect = leftmostSelect->larg;
    Assert(leftmostSelect && IsA(leftmostSelect, SelectStmt) &&
           leftmostSelect->larg == NULL);
    if (leftmostSelect->intoClause)
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("SELECT ... INTO is not allowed here"),
                 parser_errposition(pstate,
                                    exprLocation((Node *) leftmostSelect->intoClause))));

    /*
     * We need to extract ORDER BY and other top-level clauses here and not
     * let transformSetOperationTree() see them --- else it'll just recurse
     * right back here!
     */
    sortClause = stmt->sortClause;
    limitOffset = stmt->limitOffset;
    limitCount = stmt->limitCount;
    lockingClause = stmt->lockingClause;
    withClause = stmt->withClause;

    stmt->sortClause = NIL;
    stmt->limitOffset = NULL;
    stmt->limitCount = NULL;
    stmt->lockingClause = NIL;
    stmt->withClause = NULL;

    /* We don't support FOR UPDATE/SHARE with set ops at the moment. */
    if (lockingClause)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
                        LCS_asString(((LockingClause *)
                                      linitial(lockingClause))->strength))));

    /* Process the WITH clause independently of all else */
    if (withClause)
    {
        qry->hasRecursive = withClause->recursive;
        qry->cteList = transformWithClause(pstate, withClause);
        qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
    }

    /*
     * Recursively transform the components of the tree.
     */
    sostmt = castNode(SetOperationStmt,
                      transformSetOperationTree(pstate, stmt, true, NULL));
    Assert(sostmt);
    qry->setOperations = (Node *) sostmt;

    /*
     * Re-find leftmost SELECT (now it's a sub-query in rangetable)
     */
    node = sostmt->larg;
    while (node && IsA(node, SetOperationStmt))
        node = ((SetOperationStmt *) node)->larg;
    Assert(node && IsA(node, RangeTblRef));
    leftmostRTI = ((RangeTblRef *) node)->rtindex;
    leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
    Assert(leftmostQuery != NULL);

    /*
     * Generate dummy targetlist for outer query using column names of
     * leftmost select and common datatypes/collations of topmost set
     * operation.  Also make lists of the dummy vars and their names for use
     * in parsing ORDER BY.
     *
     * Note: we use leftmostRTI as the varno of the dummy variables. It
     * shouldn't matter too much which RT index they have, as long as they
     * have one that corresponds to a real RT entry; else funny things may
     * happen when the tree is mashed by rule rewriting.
     */
    qry->targetList = NIL;
    targetvars = NIL;
    targetnames = NIL;
    left_tlist = list_head(leftmostQuery->targetList);

    forthree(lct, sostmt->colTypes,
             lcm, sostmt->colTypmods,
             lcc, sostmt->colCollations)
    {
        Oid            colType = lfirst_oid(lct);
        int32        colTypmod = lfirst_int(lcm);
        Oid            colCollation = lfirst_oid(lcc);
        TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
        char       *colName;
        TargetEntry *tle;
        Var           *var;

        Assert(!lefttle->resjunk);
        colName = pstrdup(lefttle->resname);
        var = makeVar(leftmostRTI,
                      lefttle->resno,
                      colType,
                      colTypmod,
                      colCollation,
                      0);
        var->location = exprLocation((Node *) lefttle->expr);
        tle = makeTargetEntry((Expr *) var,
                              (AttrNumber) pstate->p_next_resno++,
                              colName,
                              false);
        qry->targetList = lappend(qry->targetList, tle);
        targetvars = lappend(targetvars, var);
        targetnames = lappend(targetnames, makeString(colName));
        left_tlist = lnext(left_tlist);
    }

    /*
     * As a first step towards supporting sort clauses that are expressions
     * using the output columns, generate a namespace entry that makes the
     * output columns visible.  A Join RTE node is handy for this, since we
     * can easily control the Vars generated upon matches.
     *
     * Note: we don't yet do anything useful with such cases, but at least
     * "ORDER BY upper(foo)" will draw the right error message rather than
     * "foo not found".
     */
    sv_rtable_length = list_length(pstate->p_rtable);

    jrte = addRangeTableEntryForJoin(pstate,
                                     targetnames,
                                     JOIN_INNER,
                                     targetvars,
                                     NULL,
                                     false);

    sv_namespace = pstate->p_namespace;
    pstate->p_namespace = NIL;

    /* add jrte to column namespace only */
    addRTEtoQuery(pstate, jrte, false, false, true);

    /*
     * For now, we don't support resjunk sort clauses on the output of a
     * setOperation tree --- you can only use the SQL92-spec options of
     * selecting an output column by name or number.  Enforce by checking that
     * transformSortClause doesn't add any items to tlist.
     */
    tllen = list_length(qry->targetList);

    qry->sortClause = transformSortClause(pstate,
                                          sortClause,
                                          &qry->targetList,
                                          EXPR_KIND_ORDER_BY,
                                          false /* allow SQL92 rules */ );

    /* restore namespace, remove jrte from rtable */
    pstate->p_namespace = sv_namespace;
    pstate->p_rtable = list_truncate(pstate->p_rtable, sv_rtable_length);

    if (tllen != list_length(qry->targetList))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("invalid UNION/INTERSECT/EXCEPT ORDER BY clause"),
                 errdetail("Only result column names can be used, not expressions or functions."),
                 errhint("Add the expression/function to every SELECT, or move the UNION into a FROM clause."),
                 parser_errposition(pstate,
                                    exprLocation(list_nth(qry->targetList, tllen)))));

    qry->limitOffset = transformLimitClause(pstate, limitOffset,
                                            EXPR_KIND_OFFSET, "OFFSET");
    qry->limitCount = transformLimitClause(pstate, limitCount,
                                           EXPR_KIND_LIMIT, "LIMIT");

    qry->rtable = pstate->p_rtable;
    qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    qry->hasSubLinks = pstate->p_hasSubLinks;
    qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
    qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
    qry->hasAggs = pstate->p_hasAggs;
    if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
        parseCheckAggregates(pstate, qry);

    foreach(l, lockingClause)
    {
        transformLockingClause(pstate, qry,
                               (LockingClause *) lfirst(l), false);
    }

    assign_query_collations(pstate, qry);

    return qry;
}

/*
 * transformSetOperationTree
 *        Recursively transform leaves and internal nodes of a set-op tree
 *
 * In addition to returning the transformed node, if targetlist isn't NULL
 * then we return a list of its non-resjunk TargetEntry nodes.  For a leaf
 * set-op node these are the actual targetlist entries; otherwise they are
 * dummy entries created to carry the type, typmod, collation, and location
 * (for error messages) of each output column of the set-op node.  This info
 * is needed only during the internal recursion of this function, so outside
 * callers pass NULL for targetlist.  Note: the reason for passing the
 * actual targetlist entries of a leaf node is so that upper levels can
 * replace UNKNOWN Consts with properly-coerced constants.
 */
static Node *
transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
                          bool isTopLevel, List **targetlist)
{// #lizard forgives
    bool        isLeaf;

    Assert(stmt && IsA(stmt, SelectStmt));

    /* Guard against stack overflow due to overly complex set-expressions */
    check_stack_depth();

    /*
     * Validity-check both leaf and internal SELECTs for disallowed ops.
     */
    if (stmt->intoClause)
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("INTO is only allowed on first SELECT of UNION/INTERSECT/EXCEPT"),
                 parser_errposition(pstate,
                                    exprLocation((Node *) stmt->intoClause))));

    /* We don't support FOR UPDATE/SHARE with set ops at the moment. */
    if (stmt->lockingClause)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
                        LCS_asString(((LockingClause *)
                                      linitial(stmt->lockingClause))->strength))));

    /*
     * If an internal node of a set-op tree has ORDER BY, LIMIT, FOR UPDATE,
     * or WITH clauses attached, we need to treat it like a leaf node to
     * generate an independent sub-Query tree.  Otherwise, it can be
     * represented by a SetOperationStmt node underneath the parent Query.
     */
    if (stmt->op == SETOP_NONE)
    {
        Assert(stmt->larg == NULL && stmt->rarg == NULL);
        isLeaf = true;
    }
    else
    {
        Assert(stmt->larg != NULL && stmt->rarg != NULL);
        if (stmt->sortClause || stmt->limitOffset || stmt->limitCount ||
            stmt->lockingClause || stmt->withClause)
            isLeaf = true;
        else
            isLeaf = false;
    }

    if (isLeaf)
    {
        /* Process leaf SELECT */
        Query       *selectQuery;
        char        selectName[32];
        RangeTblEntry *rte PG_USED_FOR_ASSERTS_ONLY;
        RangeTblRef *rtr;
        ListCell   *tl;

        /*
         * Transform SelectStmt into a Query.
         *
         * This works the same as SELECT transformation normally would, except
         * that we prevent resolving unknown-type outputs as TEXT.  This does
         * not change the subquery's semantics since if the column type
         * matters semantically, it would have been resolved to something else
         * anyway.  Doing this lets us resolve such outputs using
         * select_common_type(), below.
         *
         * Note: previously transformed sub-queries don't affect the parsing
         * of this sub-query, because they are not in the toplevel pstate's
         * namespace list.
         */
        selectQuery = parse_sub_analyze((Node *) stmt, pstate,
                                        NULL, false, false);

        /*
         * Check for bogus references to Vars on the current query level (but
         * upper-level references are okay). Normally this can't happen
         * because the namespace will be empty, but it could happen if we are
         * inside a rule.
         */
        if (pstate->p_namespace)
        {
            if (contain_vars_of_level((Node *) selectQuery, 1))
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                         errmsg("UNION/INTERSECT/EXCEPT member statement cannot refer to other relations of same query level"),
                         parser_errposition(pstate,
                                            locate_var_of_level((Node *) selectQuery, 1))));
        }

        /*
         * Extract a list of the non-junk TLEs for upper-level processing.
         */
        if (targetlist)
        {
            *targetlist = NIL;
            foreach(tl, selectQuery->targetList)
            {
                TargetEntry *tle = (TargetEntry *) lfirst(tl);

                if (!tle->resjunk)
                    *targetlist = lappend(*targetlist, tle);
            }
        }

        /*
         * Make the leaf query be a subquery in the top-level rangetable.
         */
        snprintf(selectName, sizeof(selectName), "*SELECT* %d",
                 list_length(pstate->p_rtable) + 1);
        rte = addRangeTableEntryForSubquery(pstate,
                                            selectQuery,
                                            makeAlias(selectName, NIL),
                                            false,
                                            false);

        /*
         * Return a RangeTblRef to replace the SelectStmt in the set-op tree.
         */
        rtr = makeNode(RangeTblRef);
        /* assume new rte is at end */
        rtr->rtindex = list_length(pstate->p_rtable);
        Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
        return (Node *) rtr;
    }
    else
    {
        /* Process an internal node (set operation node) */
        SetOperationStmt *op = makeNode(SetOperationStmt);
        List       *ltargetlist;
        List       *rtargetlist;
        ListCell   *ltl;
        ListCell   *rtl;
        const char *context;

        context = (stmt->op == SETOP_UNION ? "UNION" :
                   (stmt->op == SETOP_INTERSECT ? "INTERSECT" :
                    "EXCEPT"));

        op->op = stmt->op;
        op->all = stmt->all;

        /*
         * Recursively transform the left child node.
         */
        op->larg = transformSetOperationTree(pstate, stmt->larg,
                                             false,
                                             &ltargetlist);

        /*
         * If we are processing a recursive union query, now is the time to
         * examine the non-recursive term's output columns and mark the
         * containing CTE as having those result columns.  We should do this
         * only at the topmost setop of the CTE, of course.
         */
        if (isTopLevel &&
            pstate->p_parent_cte &&
            pstate->p_parent_cte->cterecursive)
            determineRecursiveColTypes(pstate, op->larg, ltargetlist);

        /*
         * Recursively transform the right child node.
         */
        op->rarg = transformSetOperationTree(pstate, stmt->rarg,
                                             false,
                                             &rtargetlist);

        /*
         * Verify that the two children have the same number of non-junk
         * columns, and determine the types of the merged output columns.
         */
        if (list_length(ltargetlist) != list_length(rtargetlist))
            ereport(ERROR,
                    (errcode(ERRCODE_SYNTAX_ERROR),
                     errmsg("each %s query must have the same number of columns",
                            context),
                     parser_errposition(pstate,
                                        exprLocation((Node *) rtargetlist))));

        if (targetlist)
            *targetlist = NIL;
        op->colTypes = NIL;
        op->colTypmods = NIL;
        op->colCollations = NIL;
        op->groupClauses = NIL;
        forboth(ltl, ltargetlist, rtl, rtargetlist)
        {
            TargetEntry *ltle = (TargetEntry *) lfirst(ltl);
            TargetEntry *rtle = (TargetEntry *) lfirst(rtl);
            Node       *lcolnode = (Node *) ltle->expr;
            Node       *rcolnode = (Node *) rtle->expr;
            Oid            lcoltype = exprType(lcolnode);
            Oid            rcoltype = exprType(rcolnode);
            int32        lcoltypmod = exprTypmod(lcolnode);
            int32        rcoltypmod = exprTypmod(rcolnode);
            Node       *bestexpr;
            int            bestlocation;
            Oid            rescoltype;
            int32        rescoltypmod;
            Oid            rescolcoll;

            /* select common type, same as CASE et al */
            rescoltype = select_common_type(pstate,
                                            list_make2(lcolnode, rcolnode),
                                            context,
                                            &bestexpr);
            bestlocation = exprLocation(bestexpr);
            /* if same type and same typmod, use typmod; else default */
            if (lcoltype == rcoltype && lcoltypmod == rcoltypmod)
                rescoltypmod = lcoltypmod;
            else
                rescoltypmod = -1;

            /*
             * Verify the coercions are actually possible.  If not, we'd fail
             * later anyway, but we want to fail now while we have sufficient
             * context to produce an error cursor position.
             *
             * For all non-UNKNOWN-type cases, we verify coercibility but we
             * don't modify the child's expression, for fear of changing the
             * child query's semantics.
             *
             * If a child expression is an UNKNOWN-type Const or Param, we
             * want to replace it with the coerced expression.  This can only
             * happen when the child is a leaf set-op node.  It's safe to
             * replace the expression because if the child query's semantics
             * depended on the type of this output column, it'd have already
             * coerced the UNKNOWN to something else.  We want to do this
             * because (a) we want to verify that a Const is valid for the
             * target type, or resolve the actual type of an UNKNOWN Param,
             * and (b) we want to avoid unnecessary discrepancies between the
             * output type of the child query and the resolved target type.
             * Such a discrepancy would disable optimization in the planner.
             *
             * If it's some other UNKNOWN-type node, eg a Var, we do nothing
             * (knowing that coerce_to_common_type would fail).  The planner
             * is sometimes able to fold an UNKNOWN Var to a constant before
             * it has to coerce the type, so failing now would just break
             * cases that might work.
             */
            if (lcoltype != UNKNOWNOID)
                lcolnode = coerce_to_common_type(pstate, lcolnode,
                                                 rescoltype, context);
            else if (IsA(lcolnode, Const) ||
                     IsA(lcolnode, Param))
            {
                lcolnode = coerce_to_common_type(pstate, lcolnode,
                                                 rescoltype, context);
                ltle->expr = (Expr *) lcolnode;
            }

            if (rcoltype != UNKNOWNOID)
                rcolnode = coerce_to_common_type(pstate, rcolnode,
                                                 rescoltype, context);
            else if (IsA(rcolnode, Const) ||
                     IsA(rcolnode, Param))
            {
                rcolnode = coerce_to_common_type(pstate, rcolnode,
                                                 rescoltype, context);
                rtle->expr = (Expr *) rcolnode;
            }

            /*
             * Select common collation.  A common collation is required for
             * all set operators except UNION ALL; see SQL:2008 7.13 <query
             * expression> Syntax Rule 15c.  (If we fail to identify a common
             * collation for a UNION ALL column, the curCollations element
             * will be set to InvalidOid, which may result in a runtime error
             * if something at a higher query level wants to use the column's
             * collation.)
             */
            rescolcoll = select_common_collation(pstate,
                                                 list_make2(lcolnode, rcolnode),
                                                 (op->op == SETOP_UNION && op->all));

            /* emit results */
            op->colTypes = lappend_oid(op->colTypes, rescoltype);
            op->colTypmods = lappend_int(op->colTypmods, rescoltypmod);
            op->colCollations = lappend_oid(op->colCollations, rescolcoll);

            /*
             * For all cases except UNION ALL, identify the grouping operators
             * (and, if available, sorting operators) that will be used to
             * eliminate duplicates.
             */
            if (op->op != SETOP_UNION || !op->all)
            {
                SortGroupClause *grpcl = makeNode(SortGroupClause);
                Oid            sortop;
                Oid            eqop;
                bool        hashable;
                ParseCallbackState pcbstate;

                setup_parser_errposition_callback(&pcbstate, pstate,
                                                  bestlocation);

                /* determine the eqop and optional sortop */
                get_sort_group_operators(rescoltype,
                                         false, true, false,
                                         &sortop, &eqop, NULL,
                                         &hashable);

                cancel_parser_errposition_callback(&pcbstate);

                /* we don't have a tlist yet, so can't assign sortgrouprefs */
                grpcl->tleSortGroupRef = 0;
                grpcl->eqop = eqop;
                grpcl->sortop = sortop;
                grpcl->nulls_first = false; /* OK with or without sortop */
                grpcl->hashable = hashable;

                op->groupClauses = lappend(op->groupClauses, grpcl);
            }

            /*
             * Construct a dummy tlist entry to return.  We use a SetToDefault
             * node for the expression, since it carries exactly the fields
             * needed, but any other expression node type would do as well.
             */
            if (targetlist)
            {
                SetToDefault *rescolnode = makeNode(SetToDefault);
                TargetEntry *restle;

                rescolnode->typeId = rescoltype;
                rescolnode->typeMod = rescoltypmod;
                rescolnode->collation = rescolcoll;
                rescolnode->location = bestlocation;
                restle = makeTargetEntry((Expr *) rescolnode,
                                         0, /* no need to set resno */
                                         NULL,
                                         false);
                *targetlist = lappend(*targetlist, restle);
            }
        }

        return (Node *) op;
    }
}

/*
 * Process the outputs of the non-recursive term of a recursive union
 * to set up the parent CTE's columns
 */
static void
determineRecursiveColTypes(ParseState *pstate, Node *larg, List *nrtargetlist)
{
    Node       *node;
    int            leftmostRTI;
    Query       *leftmostQuery;
    List       *targetList;
    ListCell   *left_tlist;
    ListCell   *nrtl;
    int            next_resno;

    /*
     * Find leftmost leaf SELECT
     */
    node = larg;
    while (node && IsA(node, SetOperationStmt))
        node = ((SetOperationStmt *) node)->larg;
    Assert(node && IsA(node, RangeTblRef));
    leftmostRTI = ((RangeTblRef *) node)->rtindex;
    leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
    Assert(leftmostQuery != NULL);

    /*
     * Generate dummy targetlist using column names of leftmost select and
     * dummy result expressions of the non-recursive term.
     */
    targetList = NIL;
    left_tlist = list_head(leftmostQuery->targetList);
    next_resno = 1;

    foreach(nrtl, nrtargetlist)
    {
        TargetEntry *nrtle = (TargetEntry *) lfirst(nrtl);
        TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
        char       *colName;
        TargetEntry *tle;

        Assert(!lefttle->resjunk);
        colName = pstrdup(lefttle->resname);
        tle = makeTargetEntry(nrtle->expr,
                              next_resno++,
                              colName,
                              false);
        targetList = lappend(targetList, tle);
        left_tlist = lnext(left_tlist);
    }

    /* Now build CTE's output column info using dummy targetlist */
    analyzeCTETargetList(pstate, pstate->p_parent_cte, targetList);
}


/*
 * transformUpdateStmt -
 *      transforms an update statement
 */
static Query *
transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt)
{
    Query       *qry = makeNode(Query);
    ParseNamespaceItem *nsitem;
    Node       *qual;

    qry->commandType = CMD_UPDATE;
    pstate->p_is_insert = false;

    /* process the WITH clause independently of all else */
    if (stmt->withClause)
    {
        qry->hasRecursive = stmt->withClause->recursive;
        qry->cteList = transformWithClause(pstate, stmt->withClause);
        qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
    }

    qry->resultRelation = setTargetTable(pstate, stmt->relation,
                                         stmt->relation->inh,
                                         true,
                                         ACL_UPDATE);

    /* grab the namespace item made by setTargetTable */
    nsitem = (ParseNamespaceItem *) llast(pstate->p_namespace);

    /* subqueries in FROM cannot access the result relation */
    nsitem->p_lateral_only = true;
    nsitem->p_lateral_ok = false;

    /*
     * the FROM clause is non-standard SQL syntax. We used to be able to do
     * this with REPLACE in POSTQUEL so we keep the feature.
     */
    transformFromClause(pstate, stmt->fromClause);

    /* remaining clauses can reference the result relation normally */
    nsitem->p_lateral_only = false;
    nsitem->p_lateral_ok = true;

    qual = transformWhereClause(pstate, stmt->whereClause,
                                EXPR_KIND_WHERE, "WHERE");

    qry->returningList = transformReturningList(pstate, stmt->returningList);

    /*
     * Now we are done with SELECT-like processing, and can get on with
     * transforming the target list to match the UPDATE target columns.
     */
    qry->targetList = transformUpdateTargetList(pstate, stmt->targetList);

    qry->rtable = pstate->p_rtable;
    qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

    qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
    qry->hasSubLinks = pstate->p_hasSubLinks;

    assign_query_collations(pstate, qry);

    return qry;
}

/*
 * transformUpdateTargetList -
 *    handle SET clause in UPDATE/INSERT ... ON CONFLICT UPDATE
 */
static List *
transformUpdateTargetList(ParseState *pstate, List *origTlist)
{// #lizard forgives
    List       *tlist = NIL;
    RangeTblEntry *target_rte;
    ListCell   *orig_tl;
    ListCell   *tl;
#ifdef __TBASE__
    RelationLocInfo *rd_locator_info = pstate->p_target_relation->rd_locator_info;
#endif

    tlist = transformTargetList(pstate, origTlist,
                                EXPR_KIND_UPDATE_SOURCE);

    /* Prepare to assign non-conflicting resnos to resjunk attributes */
    if (pstate->p_next_resno <= pstate->p_target_relation->rd_rel->relnatts)
        pstate->p_next_resno = pstate->p_target_relation->rd_rel->relnatts + 1;

    /* Prepare non-junk columns for assignment to target table */
    target_rte = pstate->p_target_rangetblentry;
    orig_tl = list_head(origTlist);

    foreach(tl, tlist)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(tl);
        ResTarget  *origTarget;
        int            attrno;

        if (tle->resjunk)
        {
            /*
             * Resjunk nodes need no additional processing, but be sure they
             * have resnos that do not match any target columns; else rewriter
             * or planner might get confused.  They don't need a resname
             * either.
             */
            tle->resno = (AttrNumber) pstate->p_next_resno++;
            tle->resname = NULL;
            continue;
        }
        if (orig_tl == NULL)
            elog(ERROR, "UPDATE target count mismatch --- internal error");
        origTarget = lfirst_node(ResTarget, orig_tl);

        attrno = attnameAttNum(pstate->p_target_relation,
                               origTarget->name, true);
        if (attrno == InvalidAttrNumber)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_COLUMN),
                     errmsg("column \"%s\" of relation \"%s\" does not exist",
                            origTarget->name,
                            RelationGetRelationName(pstate->p_target_relation)),
                     parser_errposition(pstate, origTarget->location)));

#ifdef __TBASE__
        /* could not update distributed columns */
        if(IS_PGXC_COORDINATOR)
        {
            if((rd_locator_info && rd_locator_info->partAttrNum != InvalidAttrNumber && rd_locator_info->partAttrNum == attrno))
            {
                bool updated = true;
                Expr *expr = tle->expr;

                if (IsA(expr, Var))
                {
                    Oid resultrel;
                    Oid varrel;
                    Var *var = (Var *)expr;
                    RangeTblEntry *rte = rt_fetch(var->varno, pstate->p_rtable);
                    char *aliasname = NULL;

                    if (rte->eref)
                        aliasname = rte->eref->aliasname;
                    
                    resultrel = RelationGetRelid(pstate->p_target_relation);
                    varrel = rte->relid;

                    if (resultrel == varrel && var->varattno == attrno)
                    {
                        if (!aliasname || strcmp(aliasname, "excluded") != 0)
                            updated = false;
                    }
                    
                }
                
                if (updated)
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                            (errmsg("Distributed column or partition column \"%s\" can't be updated in current version", origTarget->name))));
            }
            else if (RELATION_IS_INTERVAL(pstate->p_target_relation) && RelationGetPartitionColumnIndex(pstate->p_target_relation) == attrno)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                        (errmsg("Distributed column or partition column \"%s\" can't be updated in current version", origTarget->name))));
            }
        }
#endif

        updateTargetListEntry(pstate, tle, origTarget->name,
                              attrno,
                              origTarget->indirection,
                              origTarget->location);

        /* Mark the target column as requiring update permissions */
        target_rte->updatedCols = bms_add_member(target_rte->updatedCols,
                                                 attrno - FirstLowInvalidHeapAttributeNumber);

        orig_tl = lnext(orig_tl);
    }
    if (orig_tl != NULL)
        elog(ERROR, "UPDATE target count mismatch --- internal error");

    return tlist;
}

/*
 * transformReturningList -
 *    handle a RETURNING clause in INSERT/UPDATE/DELETE
 */
static List *
transformReturningList(ParseState *pstate, List *returningList)
{
    List       *rlist;
    int            save_next_resno;

    if (returningList == NIL)
        return NIL;                /* nothing to do */

    /*
     * We need to assign resnos starting at one in the RETURNING list. Save
     * and restore the main tlist's value of p_next_resno, just in case
     * someone looks at it later (probably won't happen).
     */
    save_next_resno = pstate->p_next_resno;
    pstate->p_next_resno = 1;

    /* transform RETURNING identically to a SELECT targetlist */
    rlist = transformTargetList(pstate, returningList, EXPR_KIND_RETURNING);

    /*
     * Complain if the nonempty tlist expanded to nothing (which is possible
     * if it contains only a star-expansion of a zero-column table).  If we
     * allow this, the parsed Query will look like it didn't have RETURNING,
     * with results that would probably surprise the user.
     */
    if (rlist == NIL)
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("RETURNING must have at least one column"),
                 parser_errposition(pstate,
                                    exprLocation(linitial(returningList)))));

    /* mark column origins */
    markTargetListOrigins(pstate, rlist);

    /* resolve any still-unresolved output columns as being type text */
    if (pstate->p_resolve_unknowns)
        resolveTargetListUnknowns(pstate, rlist);

    /* restore state */
    pstate->p_next_resno = save_next_resno;

    return rlist;
}


/*
 * transformDeclareCursorStmt -
 *    transform a DECLARE CURSOR Statement
 *
 * DECLARE CURSOR is like other utility statements in that we emit it as a
 * CMD_UTILITY Query node; however, we must first transform the contained
 * query.  We used to postpone that until execution, but it's really necessary
 * to do it during the normal parse analysis phase to ensure that side effects
 * of parser hooks happen at the expected time.
 */
static Query *
transformDeclareCursorStmt(ParseState *pstate, DeclareCursorStmt *stmt)
{// #lizard forgives
    Query       *result;
    Query       *query;

    /*
     * Don't allow both SCROLL and NO SCROLL to be specified
     */
    if ((stmt->options & CURSOR_OPT_SCROLL) &&
        (stmt->options & CURSOR_OPT_NO_SCROLL))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
                 errmsg("cannot specify both SCROLL and NO SCROLL")));

    /* Transform contained query, not allowing SELECT INTO */
    query = transformStmt(pstate, stmt->query);
    stmt->query = (Node *) query;

    /* Grammar should not have allowed anything but SELECT */
    if (!IsA(query, Query) ||
        query->commandType != CMD_SELECT)
        elog(ERROR, "unexpected non-SELECT command in DECLARE CURSOR");

    /*
     * We also disallow data-modifying WITH in a cursor.  (This could be
     * allowed, but the semantics of when the updates occur might be
     * surprising.)
     */
    if (query->hasModifyingCTE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("DECLARE CURSOR must not contain data-modifying statements in WITH")));

    /* FOR UPDATE and WITH HOLD are not compatible */
    if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_HOLD))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("DECLARE CURSOR WITH HOLD ... %s is not supported",
                        LCS_asString(((RowMarkClause *)
                                      linitial(query->rowMarks))->strength)),
                 errdetail("Holdable cursors must be READ ONLY.")));

    /* FOR UPDATE and SCROLL are not compatible */
    if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_SCROLL))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("DECLARE SCROLL CURSOR ... %s is not supported",
                        LCS_asString(((RowMarkClause *)
                                      linitial(query->rowMarks))->strength)),
                 errdetail("Scrollable cursors must be READ ONLY.")));

    /* FOR UPDATE and INSENSITIVE are not compatible */
    if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_INSENSITIVE))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("DECLARE INSENSITIVE CURSOR ... %s is not supported",
                        LCS_asString(((RowMarkClause *)
                                      linitial(query->rowMarks))->strength)),
                 errdetail("Insensitive cursors must be READ ONLY.")));

    /* represent the command as a utility Query */
    result = makeNode(Query);
    result->commandType = CMD_UTILITY;
    result->utilityStmt = (Node *) stmt;

    return result;
}


/*
 * transformExplainStmt -
 *    transform an EXPLAIN Statement
 *
 * EXPLAIN is like other utility statements in that we emit it as a
 * CMD_UTILITY Query node; however, we must first transform the contained
 * query.  We used to postpone that until execution, but it's really necessary
 * to do it during the normal parse analysis phase to ensure that side effects
 * of parser hooks happen at the expected time.
 */
static Query *
transformExplainStmt(ParseState *pstate, ExplainStmt *stmt)
{
    Query       *result;

    /* transform contained query, allowing SELECT INTO */
    stmt->query = (Node *) transformOptionalSelectInto(pstate, stmt->query);

    /* represent the command as a utility Query */
    result = makeNode(Query);
    result->commandType = CMD_UTILITY;
    result->utilityStmt = (Node *) stmt;

    return result;
}


/*
 * transformCreateTableAsStmt -
 *    transform a CREATE TABLE AS, SELECT ... INTO, or CREATE MATERIALIZED VIEW
 *    Statement
 *
 * As with DECLARE CURSOR and EXPLAIN, transform the contained statement now.
 */
static Query *
transformCreateTableAsStmt(ParseState *pstate, CreateTableAsStmt *stmt)
{
    Query       *result;
    Query       *query;

    /* transform contained query, not allowing SELECT INTO */
    query = transformStmt(pstate, stmt->query);
    stmt->query = (Node *) query;

    /* additional work needed for CREATE MATERIALIZED VIEW */
    if (stmt->relkind == OBJECT_MATVIEW)
    {
        /*
         * Prohibit a data-modifying CTE in the query used to create a
         * materialized view. It's not sufficiently clear what the user would
         * want to happen if the MV is refreshed or incrementally maintained.
         */
        if (query->hasModifyingCTE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("materialized views must not use data-modifying statements in WITH")));

        /*
         * Check whether any temporary database objects are used in the
         * creation query. It would be hard to refresh data or incrementally
         * maintain it if a source disappeared.
         */
        if (isQueryUsingTempRelation(query))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("materialized views must not use temporary tables or views")));

        /*
         * A materialized view would either need to save parameters for use in
         * maintaining/loading the data or prohibit them entirely.  The latter
         * seems safer and more sane.
         */
        if (query_contains_extern_params(query))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("materialized views may not be defined using bound parameters")));

        /*
         * For now, we disallow unlogged materialized views, because it seems
         * like a bad idea for them to just go to empty after a crash. (If we
         * could mark them as unpopulated, that would be better, but that
         * requires catalog changes which crash recovery can't presently
         * handle.)
         */
        if (stmt->into->rel->relpersistence == RELPERSISTENCE_UNLOGGED)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("materialized views cannot be UNLOGGED")));

        /*
         * At runtime, we'll need a copy of the parsed-but-not-rewritten Query
         * for purposes of creating the view's ON SELECT rule.  We stash that
         * in the IntoClause because that's where intorel_startup() can
         * conveniently get it from.
         */
        stmt->into->viewQuery = (Node *) copyObject(query);
    }

    /* represent the command as a utility Query */
    result = makeNode(Query);
    result->commandType = CMD_UTILITY;
    result->utilityStmt = (Node *) stmt;

    return result;
}

#ifdef PGXC
/*
 * transformExecDirectStmt -
 *    transform an EXECUTE DIRECT Statement
 *
 * Handling is depends if we should execute on nodes or on Coordinator.
 * To execute on nodes we return CMD_UTILITY query having one T_RemoteQuery node
 * with the inner statement as a sql_command.
 * If statement is to run on Coordinator we should parse inner statement and
 * analyze resulting query tree.
 */
static Query *
transformExecDirectStmt(ParseState *pstate, ExecDirectStmt *stmt)
{// #lizard forgives
    Query        *result = makeNode(Query);
    char        *query = stmt->query;
    List        *nodelist = stmt->node_names;
    RemoteQuery    *step = makeNode(RemoteQuery);
    bool        is_local = false;
    List        *raw_parsetree_list;
    ListCell    *raw_parsetree_item;
    char        *nodename;
    int            nodeIndex;
    char        nodetype;

    /* Support not available on Datanodes */
    if (IS_PGXC_DATANODE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("EXECUTE DIRECT cannot be executed on a Datanode")));

    if (list_length(nodelist) > 1)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("Support for EXECUTE DIRECT on multiple nodes is not available yet")));

    Assert(list_length(nodelist) == 1);
    Assert(IS_PGXC_COORDINATOR);

    /* There is a single element here */
    nodename = strVal(linitial(nodelist));
#ifdef XCP
    nodetype = PGXC_NODE_NONE;
    nodeIndex = PGXCNodeGetNodeIdFromName(nodename, &nodetype);
    if (nodetype == PGXC_NODE_NONE)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("PGXC Node %s: object not defined",
                        nodename)));
#else
    nodeoid = get_pgxc_nodeoid(nodename);

    if (!OidIsValid(nodeoid))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("PGXC Node %s: object not defined",
                        nodename)));

    /* Get node type and index */
    nodetype = get_pgxc_nodetype(nodeoid);
    nodeIndex = PGXCNodeGetNodeId(nodeoid, get_pgxc_nodetype(nodeoid));
#endif

    /* Check if node is requested is the self-node or not */
    if (nodetype == PGXC_NODE_COORDINATOR && nodeIndex == PGXCNodeId - 1)
        is_local = true;

    /* Transform the query into a raw parse list */
    raw_parsetree_list = pg_parse_query(query);

    /* EXECUTE DIRECT can just be executed with a single query */
    if (list_length(raw_parsetree_list) > 1)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("EXECUTE DIRECT cannot execute multiple queries")));

    /*
     * Analyze the Raw parse tree
     * EXECUTE DIRECT is restricted to one-step usage
     */
    foreach(raw_parsetree_item, raw_parsetree_list)
    {
        RawStmt   *parsetree = lfirst_node(RawStmt, raw_parsetree_item);
        List *result_list = pg_analyze_and_rewrite(parsetree, query, NULL, 0, NULL);
        result = linitial_node(Query, result_list);
    }

    /* Default list of parameters to set */
    step->sql_statement = NULL;
    step->exec_nodes = makeNode(ExecNodes);
    step->combine_type = COMBINE_TYPE_NONE;
    step->sort = NULL;
    step->read_only = true;
    step->force_autocommit = false;
    step->cursor = NULL;

    /* This is needed by executor */
    step->sql_statement = pstrdup(query);
    if (nodetype == PGXC_NODE_COORDINATOR)
        step->exec_type = EXEC_ON_COORDS;
    else
        step->exec_type = EXEC_ON_DATANODES;

    step->reduce_level = 0;
    step->base_tlist = NIL;
    step->outer_alias = NULL;
    step->inner_alias = NULL;
    step->outer_reduce_level = 0;
    step->inner_reduce_level = 0;
    step->outer_relids = NULL;
    step->inner_relids = NULL;
    step->inner_statement = NULL;
    step->outer_statement = NULL;
    step->join_condition = NULL;

    /* Change the list of nodes that will be executed for the query and others */
    step->force_autocommit = false;
    step->combine_type = COMBINE_TYPE_SAME;
    step->read_only = true;
    step->exec_direct_type = EXEC_DIRECT_NONE;

    /* Set up EXECUTE DIRECT flag */
    if (is_local)
    {
        if (result->commandType == CMD_UTILITY)
            step->exec_direct_type = EXEC_DIRECT_LOCAL_UTILITY;
        else
            step->exec_direct_type = EXEC_DIRECT_LOCAL;
    }
    else
    {
        switch(result->commandType)
        {
            case CMD_UTILITY:
                step->exec_direct_type = EXEC_DIRECT_UTILITY;
                break;
            case CMD_SELECT:
                step->exec_direct_type = EXEC_DIRECT_SELECT;
                break;
            case CMD_INSERT:
                step->exec_direct_type = EXEC_DIRECT_INSERT;
                break;
            case CMD_UPDATE:
                step->exec_direct_type = EXEC_DIRECT_UPDATE;
                break;
            case CMD_DELETE:
                step->exec_direct_type = EXEC_DIRECT_DELETE;
                break;
            default:
                Assert(0);
        }
    }

    /* Build Execute Node list, there is a unique node for the time being */
    step->exec_nodes->nodeList = lappend_int(step->exec_nodes->nodeList, nodeIndex);

    if (!is_local)
        result->utilityStmt = (Node *) step;

    /*
     * Reset the queryId since the caller would do that anyways.
     */
    result->queryId = 0;

    return result;
}

#endif

/*
 * Produce a string representation of a LockClauseStrength value.
 * This should only be applied to valid values (not LCS_NONE).
 */
const char *
LCS_asString(LockClauseStrength strength)
{
    switch (strength)
    {
        case LCS_NONE:
            Assert(false);
            break;
        case LCS_FORKEYSHARE:
            return "FOR KEY SHARE";
        case LCS_FORSHARE:
            return "FOR SHARE";
        case LCS_FORNOKEYUPDATE:
            return "FOR NO KEY UPDATE";
        case LCS_FORUPDATE:
            return "FOR UPDATE";
    }
    return "FOR some";            /* shouldn't happen */
}

/*
 * Check for features that are not supported with FOR [KEY] UPDATE/SHARE.
 *
 * exported so planner can check again after rewriting, query pullup, etc
 */
void
CheckSelectLocking(Query *qry, LockClauseStrength strength)
{// #lizard forgives
    Assert(strength != LCS_NONE);    /* else caller error */

    if (qry->setOperations)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
                        LCS_asString(strength))));
    if (qry->distinctClause != NIL)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s is not allowed with DISTINCT clause",
                        LCS_asString(strength))));
    if (qry->groupClause != NIL)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s is not allowed with GROUP BY clause",
                        LCS_asString(strength))));
    if (qry->havingQual != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s is not allowed with HAVING clause",
                        LCS_asString(strength))));
    if (qry->hasAggs)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s is not allowed with aggregate functions",
                        LCS_asString(strength))));
    if (qry->hasWindowFuncs)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s is not allowed with window functions",
                        LCS_asString(strength))));
    if (qry->hasTargetSRFs)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /*------
          translator: %s is a SQL row locking clause such as FOR UPDATE */
                 errmsg("%s is not allowed with set-returning functions in the target list",
                        LCS_asString(strength))));
}

/*
 * Transform a FOR [KEY] UPDATE/SHARE clause
 *
 * This basically involves replacing names by integer relids.
 *
 * NB: if you need to change this, see also markQueryForLocking()
 * in rewriteHandler.c, and isLockedRefname() in parse_relation.c.
 */
static void
transformLockingClause(ParseState *pstate, Query *qry, LockingClause *lc,
                       bool pushedDown)
{// #lizard forgives
    List       *lockedRels = lc->lockedRels;
    ListCell   *l;
    ListCell   *rt;
    Index        i;
    LockingClause *allrels;

    CheckSelectLocking(qry, lc->strength);

    /* make a clause we can pass down to subqueries to select all rels */
    allrels = makeNode(LockingClause);
    allrels->lockedRels = NIL;    /* indicates all rels */
    allrels->strength = lc->strength;
    allrels->waitPolicy = lc->waitPolicy;

    if (lockedRels == NIL)
    {
        /* all regular tables used in query */
        i = 0;
        foreach(rt, qry->rtable)
        {
            RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

            ++i;
            switch (rte->rtekind)
            {
                case RTE_RELATION:
                    applyLockingClause(qry, i, lc->strength, lc->waitPolicy,
                                       pushedDown);
                    rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
                    break;
                case RTE_SUBQUERY:
                    applyLockingClause(qry, i, lc->strength, lc->waitPolicy,
                                       pushedDown);

                    /*
                     * FOR UPDATE/SHARE of subquery is propagated to all of
                     * subquery's rels, too.  We could do this later (based on
                     * the marking of the subquery RTE) but it is convenient
                     * to have local knowledge in each query level about which
                     * rels need to be opened with RowShareLock.
                     */
                    transformLockingClause(pstate, rte->subquery,
                                           allrels, true);
                    break;
                default:
                    /* ignore JOIN, SPECIAL, FUNCTION, VALUES, CTE RTEs */
                    break;
            }
        }
    }
    else
    {
        /* just the named tables */
        foreach(l, lockedRels)
        {
            RangeVar   *thisrel = (RangeVar *) lfirst(l);

            /* For simplicity we insist on unqualified alias names here */
            if (thisrel->catalogname || thisrel->schemaname)
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                /*------
                  translator: %s is a SQL row locking clause such as FOR UPDATE */
                         errmsg("%s must specify unqualified relation names",
                                LCS_asString(lc->strength)),
                         parser_errposition(pstate, thisrel->location)));

            i = 0;
            foreach(rt, qry->rtable)
            {
                RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

                ++i;
                if (strcmp(rte->eref->aliasname, thisrel->relname) == 0)
                {
                    switch (rte->rtekind)
                    {
                        case RTE_RELATION:
                            applyLockingClause(qry, i, lc->strength,
                                               lc->waitPolicy, pushedDown);
                            rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
                            break;
                        case RTE_SUBQUERY:
                            applyLockingClause(qry, i, lc->strength,
                                               lc->waitPolicy, pushedDown);
                            /* see comment above */
                            transformLockingClause(pstate, rte->subquery,
                                                   allrels, true);
                            break;
                        case RTE_JOIN:
                            ereport(ERROR,
                                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            /*------
                              translator: %s is a SQL row locking clause such as FOR UPDATE */
                                     errmsg("%s cannot be applied to a join",
                                            LCS_asString(lc->strength)),
                                     parser_errposition(pstate, thisrel->location)));
                            break;
                        case RTE_FUNCTION:
                            ereport(ERROR,
                                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            /*------
                              translator: %s is a SQL row locking clause such as FOR UPDATE */
                                     errmsg("%s cannot be applied to a function",
                                            LCS_asString(lc->strength)),
                                     parser_errposition(pstate, thisrel->location)));
                            break;
                        case RTE_TABLEFUNC:
                            ereport(ERROR,
                                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            /*------
                              translator: %s is a SQL row locking clause such as FOR UPDATE */
                                     errmsg("%s cannot be applied to a table function",
                                            LCS_asString(lc->strength)),
                                     parser_errposition(pstate, thisrel->location)));
                            break;
                        case RTE_VALUES:
                            ereport(ERROR,
                                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            /*------
                              translator: %s is a SQL row locking clause such as FOR UPDATE */
                                     errmsg("%s cannot be applied to VALUES",
                                            LCS_asString(lc->strength)),
                                     parser_errposition(pstate, thisrel->location)));
                            break;
                        case RTE_CTE:
                            ereport(ERROR,
                                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            /*------
                              translator: %s is a SQL row locking clause such as FOR UPDATE */
                                     errmsg("%s cannot be applied to a WITH query",
                                            LCS_asString(lc->strength)),
                                     parser_errposition(pstate, thisrel->location)));
                            break;
                        case RTE_NAMEDTUPLESTORE:
                            ereport(ERROR,
                                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            /*------
                              translator: %s is a SQL row locking clause such as FOR UPDATE */
                                     errmsg("%s cannot be applied to a named tuplestore",
                                            LCS_asString(lc->strength)),
                                     parser_errposition(pstate, thisrel->location)));
                            break;
                        default:
                            elog(ERROR, "unrecognized RTE type: %d",
                                 (int) rte->rtekind);
                            break;
                    }
                    break;        /* out of foreach loop */
                }
            }
            if (rt == NULL)
                ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_TABLE),
                /*------
                  translator: %s is a SQL row locking clause such as FOR UPDATE */
                         errmsg("relation \"%s\" in %s clause not found in FROM clause",
                                thisrel->relname,
                                LCS_asString(lc->strength)),
                         parser_errposition(pstate, thisrel->location)));
        }
    }
}

/*
 * Record locking info for a single rangetable item
 */
void
applyLockingClause(Query *qry, Index rtindex,
                   LockClauseStrength strength, LockWaitPolicy waitPolicy,
                   bool pushedDown)
{
    RowMarkClause *rc;

    Assert(strength != LCS_NONE);    /* else caller error */

    /* If it's an explicit clause, make sure hasForUpdate gets set */
    if (!pushedDown)
        qry->hasForUpdate = true;

    /* Check for pre-existing entry for same rtindex */
    if ((rc = get_parse_rowmark(qry, rtindex)) != NULL)
    {
        /*
         * If the same RTE is specified with more than one locking strength,
         * use the strongest.  (Reasonable, since you can't take both a shared
         * and exclusive lock at the same time; it'll end up being exclusive
         * anyway.)
         *
         * Similarly, if the same RTE is specified with more than one lock
         * wait policy, consider that NOWAIT wins over SKIP LOCKED, which in
         * turn wins over waiting for the lock (the default).  This is a bit
         * more debatable but raising an error doesn't seem helpful. (Consider
         * for instance SELECT FOR UPDATE NOWAIT from a view that internally
         * contains a plain FOR UPDATE spec.)  Having NOWAIT win over SKIP
         * LOCKED is reasonable since the former throws an error in case of
         * coming across a locked tuple, which may be undesirable in some
         * cases but it seems better than silently returning inconsistent
         * results.
         *
         * And of course pushedDown becomes false if any clause is explicit.
         */
        rc->strength = Max(rc->strength, strength);
        rc->waitPolicy = Max(rc->waitPolicy, waitPolicy);
        rc->pushedDown &= pushedDown;
        return;
    }

    /* Make a new RowMarkClause */
    rc = makeNode(RowMarkClause);
    rc->rti = rtindex;
    rc->strength = strength;
    rc->waitPolicy = waitPolicy;
    rc->pushedDown = pushedDown;
    qry->rowMarks = lappend(qry->rowMarks, rc);
}

/*
 * Coverage testing for raw_expression_tree_walker().
 *
 * When enabled, we run raw_expression_tree_walker() over every DML statement
 * submitted to parse analysis.  Without this provision, that function is only
 * applied in limited cases involving CTEs, and we don't really want to have
 * to test everything inside as well as outside a CTE.
 */
#ifdef RAW_EXPRESSION_COVERAGE_TEST

static bool
test_raw_expression_coverage(Node *node, void *context)
{
    if (node == NULL)
        return false;
    return raw_expression_tree_walker(node,
                                      test_raw_expression_coverage,
                                      context);
}

#endif                            /* RAW_EXPRESSION_COVERAGE_TEST */
#ifdef __TBASE__
List *
transformInsertValuesIntoCopyFrom(List *plantree_list, InsertStmt *stmt, bool *success,
                                            char *transform_string, Query    *query)
{
    List       *stmt_list = NULL;
    CopyStmt   *copy = makeNode(CopyStmt);

    /* copy from */
    copy->is_from = true; 

    copy->relation = stmt->relation;

    copy->filename = transform_string;

    copy->data_list = stmt->data_list;

    copy->ncolumns = stmt->ninsert_columns;

    copy->ndatarows = stmt->ndatarows;

    /* insert into xxx(c2,c4,c6) to copy xxx(c2,c4,c6) */
    copy->attlist = NULL;
    /*
     * INSERT will expand the targetlist according to the target relation's
     * physical targetlist.
     * insert into xxx(c3,c2,c4)----> insert into xxx(c1,c2,c3,c4)
     * we transform any form of 'insert into' to copy xxx from ...
     *
     * if we write the values of insert_into into file, we also need this form
     */
    if (stmt->cols)
    {
        ListCell *cell;

        foreach(cell, stmt->cols)
        {
            ResTarget *target = (ResTarget *)lfirst(cell);

            Value *v = makeString(target->name);

            copy->attlist = lappend(copy->attlist, v);
        }
    }

    copy->insert_into = true;
    
    {
        PlannedStmt *planstmt = NULL;

        planstmt = makeNode(PlannedStmt);
        planstmt->commandType = CMD_UTILITY;
        planstmt->canSetTag = query->canSetTag;
        planstmt->utilityStmt = (Node *)copy;
        planstmt->stmt_location = query->stmt_location;
        planstmt->stmt_len = query->stmt_len;

        stmt_list = lappend(stmt_list, planstmt);

        if (success)
        {
            *success = true;
        }
    }

    return stmt_list;
}
#endif
