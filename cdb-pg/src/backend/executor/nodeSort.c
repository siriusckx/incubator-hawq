/*-------------------------------------------------------------------------
 *
 * nodeSort.c
 *	  Routines to handle sorting of relations.
 *
 * Portions Copyright (c) 2007-2008, Greenplum inc
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeSort.c,v 1.58 2006/10/04 00:29:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeSort.h"
#include "lib/stringinfo.h"             /* StringInfo */
#include "miscadmin.h"
#include "utils/tuplesort.h"
#include "cdb/cdbvars.h" /* CDB *//* gp_sort_flags */

static void ExecSortExplainEnd(PlanState *planstate, struct StringInfoData *buf);

/* ----------------------------------------------------------------
 *		ExecSort
 *
 *		Sorts tuples from the outer subtree of the node using tuplesort,
 *		which saves the results in a temporary file or memory. After the
 *		initial call, returns a tuple from the file with each call.
 *
 *		Conditions:
 *		  -- none.
 *
 *		Initial States:
 *		  -- the outer child is prepared to return the first tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecSort(SortState *node)
{
	EState	   *estate;
	ScanDirection dir;
	Tuplesortstate *tuplesortstate = NULL;
	Tuplesortstate_mk *tuplesortstate_mk = NULL;
	TupleTableSlot *slot;
	Sort 		*plannode;

	/*
	 * get state info from node
	 */
	SO1_printf("ExecSort: %s\n",
			   "entering routine");

	estate = node->ss.ps.state;
	dir = estate->es_direction;

	if(gp_enable_mk_sort)
		tuplesortstate_mk = (Tuplesortstate_mk *) node->tuplesortstate;
	else
		tuplesortstate = (Tuplesortstate *) node->tuplesortstate;

	/*
	 * In Window node, we might need to call ExecSort again even when
	 * the last tuple in the Sort has been retrieved. Since we might
	 * eager free the tuplestore, the tuplestorestate could be NULL.
	 * We simply return NULL in this case.
	 */
	if (node->sort_Done &&
		((gp_enable_mk_sort && tuplesortstate_mk == NULL) ||
		 (!gp_enable_mk_sort && tuplesortstate == NULL)))
	{
		return NULL;
	}

	plannode = (Sort *) node->ss.ps.plan;

	/*
	 * If first time through, read all tuples from outer plan and pass them to
	 * tuplesort.c. Subsequent calls just fetch tuples from tuplesort.
	 */

	if (!node->sort_Done)
	{
		PlanState  *outerNode;
		TupleDesc	tupDesc;

		SO1_printf("ExecSort: %s\n",
				   "sorting subplan");

		/*
		 * Want to scan subplan in the forward direction while creating the
		 * sorted data.
		 */
		estate->es_direction = ForwardScanDirection;

		/*
		 * Initialize tuplesort module.
		 */
		SO1_printf("ExecSort: %s\n",
				   "calling tuplesort_begin");

		outerNode = outerPlanState(node);
		tupDesc = ExecGetResultType(outerNode);

		if(plannode->share_type == SHARE_SORT_XSLICE)
		{
			char rwfile_prefix[100];
			if(plannode->driver_slice != currentSliceId)
			{
				elog(LOG, "Sort exec on CrossSlice, current slice %d", currentSliceId);
				return NULL;
			}

			shareinput_create_bufname_prefix(rwfile_prefix, sizeof(rwfile_prefix), plannode->share_id);
			elog(LOG, "Sort node create shareinput rwfile %s", rwfile_prefix);

			if(gp_enable_mk_sort)
				tuplesortstate_mk = tuplesort_begin_heap_file_readerwriter_mk(
					rwfile_prefix, true,
					tupDesc,
					plannode->numCols,
					plannode->sortOperators,
					plannode->sortColIdx,
					PlanStateOperatorMemKB((PlanState *) node),
					true
					); 
			else
				tuplesortstate = tuplesort_begin_heap_file_readerwriter(
					rwfile_prefix, true,
					tupDesc,
					plannode->numCols,
					plannode->sortOperators,
					plannode->sortColIdx,
					PlanStateOperatorMemKB((PlanState *) node),
					true
					); 
		}
		else
		{
			if(gp_enable_mk_sort)
				tuplesortstate_mk = tuplesort_begin_heap_mk(tupDesc,
						plannode->numCols,
						plannode->sortOperators,
						plannode->sortColIdx,
						PlanStateOperatorMemKB((PlanState *) node),
						node->randomAccess);
			else
				tuplesortstate = tuplesort_begin_heap(tupDesc,
						plannode->numCols,
						plannode->sortOperators,
						plannode->sortColIdx,
						PlanStateOperatorMemKB((PlanState *) node),
						node->randomAccess);
		}

		if(gp_enable_mk_sort)
			node->tuplesortstate = (void *) tuplesortstate_mk;
		else
			node->tuplesortstate = (void *) tuplesortstate;

		/* CDB */
		{
			ExprContext *econtext = node->ss.ps.ps_ExprContext;
			bool 		isNull;
			int64 		limit = 0;
			int64 		offset = 0;
			int 		unique = 0;
			int 		sort_flags = gp_sort_flags; /* get the guc */
			int         maxdistinct = gp_sort_max_distinct; /* get the guc */

			if (node->limitCount)
			{
				limit =
						DatumGetInt64(
								ExecEvalExprSwitchContext(node->limitCount,
														  econtext,
														  &isNull,
														  NULL));
				/* Interpret NULL limit as no limit */
				if (isNull)
					limit = 0;
				else if (limit < 0)
					limit = 0;

			}
			if (node->limitOffset)
			{
				offset =
						DatumGetInt64(
								ExecEvalExprSwitchContext(node->limitOffset,
														  econtext,
														  &isNull,
														  NULL));
				/* Interpret NULL offset as no offset */
				if (isNull)
					offset = 0;
				else if (offset < 0)
					offset = 0;

			}

			if (node->noduplicates)
				unique = 1;
			
			if(gp_enable_mk_sort)
				cdb_tuplesort_init_mk(tuplesortstate_mk, offset, limit, unique, sort_flags, maxdistinct);
			else
				cdb_tuplesort_init(tuplesortstate, offset, limit, unique, sort_flags, maxdistinct);
		}

		/* If EXPLAIN ANALYZE, share our Instrumentation object with sort. */
		if(gp_enable_mk_sort)
		{
			if (node->ss.ps.instrument)
				tuplesort_set_instrument_mk(tuplesortstate_mk,
						node->ss.ps.instrument,
						node->ss.ps.cdbexplainbuf);

			tuplesort_set_gpmon_mk(tuplesortstate_mk, &node->ss.ps.gpmon_pkt, 
					&node->ss.ps.gpmon_plan_tick);
		}
		else
		{
			if (node->ss.ps.instrument)
				tuplesort_set_instrument(tuplesortstate,
						node->ss.ps.instrument,
						node->ss.ps.cdbexplainbuf);

			tuplesort_set_gpmon(tuplesortstate, &node->ss.ps.gpmon_pkt, 
					&node->ss.ps.gpmon_plan_tick);
		}


		/*
		 * Scan the subplan and feed all the tuples to tuplesort.
		 */
		for (;;)
		{
			slot = ExecProcNode(outerNode);

			if (TupIsNull(slot))
				break;

                        CheckSendPlanStateGpmonPkt(&node->ss.ps);
			if(gp_enable_mk_sort)
				tuplesort_puttupleslot_mk(tuplesortstate_mk, slot);
			else
				tuplesort_puttupleslot(tuplesortstate, slot);
		}

		/*
		 * Complete the sort.
		 */
		if(gp_enable_mk_sort)
			tuplesort_performsort_mk(tuplesortstate_mk);
		else
			tuplesort_performsort(tuplesortstate);

                CheckSendPlanStateGpmonPkt(&node->ss.ps);
		/*
		 * restore to user specified direction
		 */
		estate->es_direction = dir;

		/*
		 * finally set the sorted flag to true
		 */
		node->sort_Done = true;
		SO1_printf("ExecSort: %s\n", "sorting done");

		/* for share input, do not need to return any tuple */
		if(plannode->share_type != SHARE_NOTSHARED) 
		{
			Assert(plannode->share_type == SHARE_SORT || plannode->share_type == SHARE_SORT_XSLICE);

			if(plannode->share_type == SHARE_SORT_XSLICE)
			{
				if(plannode->driver_slice == currentSliceId)
				{
					if(gp_enable_mk_sort)
						tuplesort_flush_mk(tuplesortstate_mk);
					else
						tuplesort_flush(tuplesortstate);

					node->share_lk_ctxt = shareinput_writer_notifyready(plannode->share_id, plannode->nsharer_xslice,
							estate->es_plannedstmt->planGen);
				}
			}

			return NULL;
		}
	}

	if(plannode->share_type != SHARE_NOTSHARED)
		return NULL;
				
	SO1_printf("ExecSort: %s\n",
			   "retrieving tuple from tuplesort");

	/*
	 * Get the first or next tuple from tuplesort. Returns NULL if no more
	 * tuples.
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
	if(gp_enable_mk_sort)
		(void) tuplesort_gettupleslot_mk(tuplesortstate_mk,
				ScanDirectionIsForward(dir),
				slot);
	else
		(void) tuplesort_gettupleslot(tuplesortstate,
				ScanDirectionIsForward(dir),
				slot);

	if (TupIsNull(slot) && !node->ss.ps.delayEagerFree)
	{
		ExecEagerFreeSort(node);
	}

	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitSort
 *
 *		Creates the run-time state information for the sort node
 *		produced by the planner and initializes its outer subtree.
 * ----------------------------------------------------------------
 */
SortState *
ExecInitSort(Sort *node, EState *estate, int eflags)
{
	SortState  *sortstate;

	SO1_printf("ExecInitSort: %s\n",
			   "initializing sort node");

	/*
	 * create state structure
	 */
	sortstate = makeNode(SortState);
	sortstate->ss.ps.plan = (Plan *) node;
	sortstate->ss.ps.state = estate;

	/*
	 * We must have random access to the sort output to do backward scan or
	 * mark/restore.  We also prefer to materialize the sort output if we
	 * might be called on to rewind and replay it many times.
	 */
	sortstate->randomAccess = (eflags & (EXEC_FLAG_REWIND |
										 EXEC_FLAG_BACKWARD |
										 EXEC_FLAG_MARK)) != 0;

	/* If the sort is shared, we need random access */
	if(node->share_type != SHARE_NOTSHARED) 
		sortstate->randomAccess = true;

	sortstate->sort_Done = false;
	sortstate->tuplesortstate = NULL;
	sortstate->share_lk_ctxt = NULL;

	/* CDB */

	/* BUT:
	 * The LIMIT optimizations requires exprcontext in which to
	 * evaluate the limit/offset parameters.
	 */
	ExecAssignExprContext(estate, &sortstate->ss.ps);

	/* CDB */ /* evaluate a limit as part of the sort */
	{
		/* pass node state to sort state */
		sortstate->limitOffset = ExecInitExpr((Expr *) node->limitOffset,
											  (PlanState *) sortstate);
		sortstate->limitCount = ExecInitExpr((Expr *) node->limitCount,
											 (PlanState *) sortstate);
		sortstate->noduplicates = node->noduplicates;
	}

	/*
	 * Miscellaneous initialization
	 *
	 * Sort nodes don't initialize their ExprContexts because they never call
	 * ExecQual or ExecProject.
	 */

#define SORT_NSLOTS 2

	/*
	 * tuple table initialization
	 *
	 * sort nodes only return scan tuples from their sorted relation.
	 */
	ExecInitResultTupleSlot(estate, &sortstate->ss.ps);
	sortstate->ss.ss_ScanTupleSlot = ExecInitExtraTupleSlot(estate);

	/* 
	 * CDB: Offer extra info for EXPLAIN ANALYZE.
	 */
	if (estate->es_instrument)
	{
		/* Allocate string buffer. */
		sortstate->ss.ps.cdbexplainbuf = makeStringInfo();

		/* Request a callback at end of query. */
		sortstate->ss.ps.cdbexplainfun = ExecSortExplainEnd;
	}

	/*
	 * If eflag contains EXEC_FLAG_REWIND or EXEC_FLAG_BACKWARD or EXEC_FLAG_MARK,
	 * then this node is not eager free safe.
	 */
	sortstate->ss.ps.delayEagerFree =
		((eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) != 0);

	/*
	 * initialize child nodes
	 *
	 * We shield the child node from the need to support REWIND, BACKWARD, or
	 * MARK/RESTORE.
	 */
	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	outerPlanState(sortstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * If the child node of a Material is a Motion, then this Material node is
	 * not eager free safe.
	 */
	if (IsA(outerPlan((Plan *)node), Motion))
	{
		sortstate->ss.ps.delayEagerFree = true;
	}

	/*
	 * initialize tuple type.  no need to initialize projection info because
	 * this node doesn't do projections.
	 */
	ExecAssignResultTypeFromTL(&sortstate->ss.ps);
	ExecAssignScanTypeFromOuterPlan(&sortstate->ss);
	sortstate->ss.ps.ps_ProjInfo = NULL;

	if(node->share_type != SHARE_NOTSHARED)
	{
		ShareNodeEntry *snEntry = ExecGetShareNodeEntry(estate, node->share_id, true);
		snEntry->sharePlan = (Node *)node;
		snEntry->shareState = (Node *)sortstate;
	}

	SO1_printf("ExecInitSort: %s\n",
			   "sort node initialized");

	initGpmonPktForSort((Plan *)node, &sortstate->ss.ps.gpmon_pkt, estate);

	return sortstate;
}

int
ExecCountSlotsSort(Sort *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) +
		SORT_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndSort(node)
 * ----------------------------------------------------------------
 */
void
ExecEndSort(SortState *node)
{
	SO1_printf("ExecEndSort: %s\n",
			   "shutting down sort node");

	ExecEagerFreeSort(node);

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));

	SO1_printf("ExecEndSort: %s\n",
			   "sort node shutdown");

	EndPlanStateGpmonPkt(&node->ss.ps);
}

/* ----------------------------------------------------------------
 *		ExecSortMarkPos
 *
 *		Calls tuplesort to save the current position in the sorted file.
 * ----------------------------------------------------------------
 */
void
ExecSortMarkPos(SortState *node)
{
	/*
	 * if we haven't sorted yet, just return
	 */
	if (!node->sort_Done)
		return;

	if(gp_enable_mk_sort)
		tuplesort_markpos_mk((Tuplesortstate_mk *) node->tuplesortstate);
	else
		tuplesort_markpos((Tuplesortstate *) node->tuplesortstate);

}

/* ----------------------------------------------------------------
 *		ExecSortRestrPos
 *
 *		Calls tuplesort to restore the last saved sort file position.
 * ----------------------------------------------------------------
 */
void
ExecSortRestrPos(SortState *node)
{
	/*
	 * if we haven't sorted yet, just return.
	 */
	if (!node->sort_Done)
		return;

	/*
	 * restore the scan to the previously marked position
	 */
	if(gp_enable_mk_sort)
		tuplesort_restorepos_mk((Tuplesortstate_mk *) node->tuplesortstate);
	else
		tuplesort_restorepos((Tuplesortstate *) node->tuplesortstate);
}

void
ExecReScanSort(SortState *node, ExprContext *exprCtxt)
{
	/*
	 * If we haven't sorted yet, just return. If outerplan' chgParam is not
	 * NULL then it will be re-scanned by ExecProcNode, else - no reason to
	 * re-scan it at all.
	 */
	if (!node->sort_Done)
		return;

	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * If subnode is to be rescanned then we forget previous sort results; we
	 * have to re-read the subplan and re-sort.
	 *
	 * Otherwise we can just rewind and rescan the sorted output.
	 */
	if (((PlanState *) node)->lefttree->chgParam != NULL ||
		!node->randomAccess ||
		node->tuplesortstate == NULL)
	{
		node->sort_Done = false;
		if (node->tuplesortstate != NULL)
		{
			if(gp_enable_mk_sort)
				tuplesort_end_mk((Tuplesortstate_mk *) node->tuplesortstate);
			else
				tuplesort_end((Tuplesortstate *) node->tuplesortstate);
			node->tuplesortstate = NULL;
		}

		/*
		 * if chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.
		 */
		if (((PlanState *) node)->lefttree->chgParam == NULL)
			ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
	}
	else
	{
		if(gp_enable_mk_sort)
			tuplesort_rescan_mk((Tuplesortstate_mk *) node->tuplesortstate);
		else
			tuplesort_rescan((Tuplesortstate *) node->tuplesortstate);
	}
}


/*
 * ExecSortExplainEnd
 *      Called before ExecutorEnd to finish EXPLAIN ANALYZE reporting.
 */
void
ExecSortExplainEnd(PlanState *planstate, struct StringInfoData *buf)
{
	SortState *sortstate = (SortState *)planstate;
	
	if (sortstate->tuplesortstate != NULL)
	{
		if (gp_enable_mk_sort)
			tuplesort_finalize_stats_mk((Tuplesortstate_mk *)sortstate->tuplesortstate);
		else
			tuplesort_finalize_stats((Tuplesortstate *)sortstate->tuplesortstate);
	}
}                               /* ExecSortExplainEnd */

void
initGpmonPktForSort(Plan *planNode, gpmon_packet_t *gpmon_pkt, EState *estate)
{
	Assert(planNode != NULL && gpmon_pkt != NULL && IsA(planNode, Sort));
	
	{
		Assert(GPMON_SORT_TOTAL <= (int)GPMON_QEXEC_M_COUNT);

		InitPlanNodeGpmonPkt(planNode, gpmon_pkt, estate, PMNT_Sort,
							 (int64)planNode->plan_rows, 
							 NULL); 
	}
}

void
ExecEagerFreeSort(SortState *node)
{
	/* clean out the tuple table */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	if (node->tuplesortstate != NULL)
	{
		Sort *sort = (Sort *) node->ss.ps.plan;

		if(sort->share_type == SHARE_SORT_XSLICE)
			shareinput_writer_waitdone(node->share_lk_ctxt, sort->share_id, sort->nsharer_xslice);

		if(gp_enable_mk_sort)
			tuplesort_end_mk((Tuplesortstate_mk *) node->tuplesortstate);
		else
			tuplesort_end((Tuplesortstate *) node->tuplesortstate);

		node->tuplesortstate = NULL;
	}
}


