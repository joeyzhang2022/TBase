/*-------------------------------------------------------------------------
 *
 * procarray.c
 *      POSTGRES process array code.
 *
 *
 * This module maintains arrays of the PGPROC and PGXACT structures for all
 * active backends.  Although there are several uses for this, the principal
 * one is as a means of determining the set of currently running transactions.
 *
 * Because of various subtle race conditions it is critical that a backend
 * hold the correct locks while setting or clearing its MyPgXact->xid field.
 * See notes in src/backend/access/transam/README.
 *
 * The process arrays now also include structures representing prepared
 * transactions.  The xid and subxids fields of these are valid, as are the
 * myProcLocks lists.  They can be distinguished from regular backend PGPROCs
 * at need by checking for pid == 0.
 *
#ifdef PGXC
 * For Postgres-XC, there is some special handling for ANALYZE.
 * An XID for a local ANALYZE command will never involve other nodes.
 * Also, ANALYZE may run for a long time, affecting snapshot xmin values
 * on other nodes unnecessarily.  We want to exclude the XID
 * in global snapshots, but include it in local ones. As a result,
 * these are tracked in shared memory separately.
#endif
 *
 * During hot standby, we also keep a list of XIDs representing transactions
 * that are known to be running in the master (or more precisely, were running
 * as of the current point in the WAL stream).  This list is kept in the
 * KnownAssignedXids array, and is updated by watching the sequence of
 * arriving XIDs.  This is necessary because if we leave those XIDs out of
 * snapshots taken for standby queries, then they will appear to be already
 * complete, leading to MVCC failures.  Note that in hot standby, the PGPROC
 * array represents standby processes, which by definition are not running
 * transactions that have XIDs.
 *
 * It is perhaps possible for a backend on the master to terminate without
 * writing an abort record for its transaction.  While that shouldn't really
 * happen, it would tie up KnownAssignedXids indefinitely, so we protect
 * ourselves by pruning the array when a valid list of running XIDs arrives.
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 *
 * IDENTIFICATION
 *      src/backend/storage/ipc/procarray.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <unistd.h>

#include <signal.h>

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "postmaster/clustermon.h"
#include "pgstat.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#ifdef PGXC
#include "pgxc/pgxc.h"
#include "access/gtm.h"
#include "storage/ipc.h"
#include "utils/guc.h"
/* PGXC_DATANODE */
#include "postmaster/autovacuum.h"
#include "utils/memutils.h"
#endif

#ifdef _MIGRATE_
#include "pgxc/shardmap.h"
#endif
#ifdef __TBASE__
#include "tcop/tcopprot.h"
#endif

#ifdef __TBASE__


#define TIMESTAMP_SHIFT (1000*1000L)


#endif
/* Our shared memory area */
typedef struct ProcArrayStruct
{
    int            numProcs;        /* number of valid procs entries */
    int            maxProcs;        /* allocated size of procs array */

    /*
     * Known assigned XIDs handling
     */
    int            maxKnownAssignedXids;    /* allocated size of array */
    int            numKnownAssignedXids;    /* current # of valid entries */
    int            tailKnownAssignedXids;    /* index of oldest valid element */
    int            headKnownAssignedXids;    /* index of newest element, + 1 */
    slock_t        known_assigned_xids_lck;    /* protects head/tail pointers */

    /*
     * Highest subxid that has been removed from KnownAssignedXids array to
     * prevent overflow; or InvalidTransactionId if none.  We track this for
     * similar reasons to tracking overflowing cached subxids in PGXACT
     * entries.  Must hold exclusive ProcArrayLock to change this, and shared
     * lock to read it.
     */
    TransactionId lastOverflowedXid;

    /* oldest xmin of any replication slot */
    TransactionId replication_slot_xmin;
    /* oldest catalog xmin of any replication slot */
    TransactionId replication_slot_catalog_xmin;

    /* indexes into allPgXact[], has PROCARRAY_MAXPROCS entries */
    int            pgprocnos[FLEXIBLE_ARRAY_MEMBER];
} ProcArrayStruct;

static ProcArrayStruct *procArray;

static PGPROC *allProcs;
static PGXACT *allPgXact;

/*
 * Bookkeeping for tracking emulated transactions in recovery
 */
static TransactionId *KnownAssignedXids;
static bool *KnownAssignedXidsValid;
static TransactionId latestObservedXid = InvalidTransactionId;

/*
 * If we're in STANDBY_SNAPSHOT_PENDING state, standbySnapshotPendingXmin is
 * the highest xid that might still be running that we don't have in
 * KnownAssignedXids.
 */
static TransactionId standbySnapshotPendingXmin;

#ifdef XIDCACHE_DEBUG

/* counters for XidCache measurement */
static long xc_by_recent_xmin = 0;
static long xc_by_known_xact = 0;
static long xc_by_my_xact = 0;
static long xc_by_latest_xid = 0;
static long xc_by_main_xid = 0;
static long xc_by_child_xid = 0;
static long xc_by_known_assigned = 0;
static long xc_no_overflow = 0;
static long xc_slow_answer = 0;

#define xc_by_recent_xmin_inc()        (xc_by_recent_xmin++)
#define xc_by_known_xact_inc()        (xc_by_known_xact++)
#define xc_by_my_xact_inc()            (xc_by_my_xact++)
#define xc_by_latest_xid_inc()        (xc_by_latest_xid++)
#define xc_by_main_xid_inc()        (xc_by_main_xid++)
#define xc_by_child_xid_inc()        (xc_by_child_xid++)
#define xc_by_known_assigned_inc()    (xc_by_known_assigned++)
#define xc_no_overflow_inc()        (xc_no_overflow++)
#define xc_slow_answer_inc()        (xc_slow_answer++)

static void DisplayXidCache(void);
#else                            /* !XIDCACHE_DEBUG */

#define xc_by_recent_xmin_inc()        ((void) 0)
#define xc_by_known_xact_inc()        ((void) 0)
#define xc_by_my_xact_inc()            ((void) 0)
#define xc_by_latest_xid_inc()        ((void) 0)
#define xc_by_main_xid_inc()        ((void) 0)
#define xc_by_child_xid_inc()        ((void) 0)
#define xc_by_known_assigned_inc()    ((void) 0)
#define xc_no_overflow_inc()        ((void) 0)
#define xc_slow_answer_inc()        ((void) 0)
#endif                            /* XIDCACHE_DEBUG */

#ifdef PGXC  /* PGXC_DATANODE */

static void GetPGXCSnapshotData(Snapshot snapshot, bool latest);

typedef struct
{
    /* Global snapshot data */
    SnapshotSource snapshot_source;
    TransactionId gxmin;
    TransactionId gxmax;
    GlobalTimestamp gts;
    int gxcnt;
    int max_gxcnt;
    TransactionId *gxip;
} GlobalSnapshotData;

GlobalSnapshotData globalSnapshot = {
    SNAPSHOT_UNDEFINED,
    InvalidTransactionId,
    InvalidTransactionId,
    InvalidGlobalTimestamp,
    0,
    0,
    NULL
};
static void GetGlobalTimestampFromGTM(Snapshot snapshot);
static void
GetGlobalTimestampFromGlobalSnapshot(Snapshot snapshot);
#if 0
static void GetSnapshotFromGlobalSnapshot(Snapshot snapshot);
static void GetSnapshotDataFromGTM(Snapshot snapshot);
#endif
#endif

/* Primitives for KnownAssignedXids array handling for standby */
static void KnownAssignedXidsCompress(bool force);
static void KnownAssignedXidsAdd(TransactionId from_xid, TransactionId to_xid,
                     bool exclusive_lock);
static bool KnownAssignedXidsSearch(TransactionId xid, bool remove);
static bool KnownAssignedXidExists(TransactionId xid);
static void KnownAssignedXidsRemove(TransactionId xid);
static void KnownAssignedXidsRemoveTree(TransactionId xid, int nsubxids,
                            TransactionId *subxids);
static void KnownAssignedXidsRemovePreceding(TransactionId xid);
static int    KnownAssignedXidsGet(TransactionId *xarray, TransactionId xmax);
static int KnownAssignedXidsGetAndSetXmin(TransactionId *xarray,
                               TransactionId *xmin,
                               TransactionId xmax);
static TransactionId KnownAssignedXidsGetOldestXmin(void);
static void KnownAssignedXidsDisplay(int trace_level);
static void KnownAssignedXidsReset(void);
static inline void ProcArrayEndTransactionInternal(PGPROC *proc,
                                PGXACT *pgxact, TransactionId latestXid);
static void ProcArrayGroupClearXid(PGPROC *proc, TransactionId latestXid);

#ifdef XCP
int    GlobalSnapshotSource;
#endif

/*
 * Report shared-memory space needed by CreateSharedProcArray.
 */
Size
ProcArrayShmemSize(void)
{
    Size        size;

    /* Size of the ProcArray structure itself */
#define PROCARRAY_MAXPROCS    (MaxBackends + max_prepared_xacts)

    size = offsetof(ProcArrayStruct, pgprocnos);
    size = add_size(size, mul_size(sizeof(int), PROCARRAY_MAXPROCS));

    /*
     * During Hot Standby processing we have a data structure called
     * KnownAssignedXids, created in shared memory. Local data structures are
     * also created in various backends during GetSnapshotData(),
     * TransactionIdIsInProgress() and GetRunningTransactionData(). All of the
     * main structures created in those functions must be identically sized,
     * since we may at times copy the whole of the data structures around. We
     * refer to this size as TOTAL_MAX_CACHED_SUBXIDS.
     *
     * Ideally we'd only create this structure if we were actually doing hot
     * standby in the current run, but we don't know that yet at the time
     * shared memory is being set up.
     */
#define TOTAL_MAX_CACHED_SUBXIDS \
    ((PGPROC_MAX_CACHED_SUBXIDS + 1) * PROCARRAY_MAXPROCS)

    if (EnableHotStandby)
    {
        size = add_size(size,
                        mul_size(sizeof(TransactionId),
                                 TOTAL_MAX_CACHED_SUBXIDS));
        size = add_size(size,
                        mul_size(sizeof(bool), TOTAL_MAX_CACHED_SUBXIDS));
    }

    return size;
}

/*
 * Initialize the shared PGPROC array during postmaster startup.
 */
void
CreateSharedProcArray(void)
{
    bool        found;

    /* Create or attach to the ProcArray shared structure */
    procArray = (ProcArrayStruct *)
        ShmemInitStruct("Proc Array",
                        add_size(offsetof(ProcArrayStruct, pgprocnos),
                                 mul_size(sizeof(int),
                                          PROCARRAY_MAXPROCS)),
                        &found);

    if (!found)
    {
        /*
         * We're the first - initialize.
         */
        procArray->numProcs = 0;
        procArray->maxProcs = PROCARRAY_MAXPROCS;
        procArray->replication_slot_xmin = InvalidTransactionId;
        procArray->maxKnownAssignedXids = TOTAL_MAX_CACHED_SUBXIDS +
            CONTROL_INTERVAL;
        procArray->numKnownAssignedXids = 0;
        procArray->tailKnownAssignedXids = 0;
        procArray->headKnownAssignedXids = 0;
        SpinLockInit(&procArray->known_assigned_xids_lck);
        procArray->lastOverflowedXid = InvalidTransactionId;
    }

    allProcs = ProcGlobal->allProcs;
    allPgXact = ProcGlobal->allPgXact;

    /* Create or attach to the KnownAssignedXids arrays too, if needed */
    if (EnableHotStandby)
    {
        KnownAssignedXids = (TransactionId *)
            ShmemInitStruct("KnownAssignedXids",
                            mul_size(sizeof(TransactionId),
                                     procArray->maxKnownAssignedXids),
                            &found);
        KnownAssignedXidsValid = (bool *)
            ShmemInitStruct("KnownAssignedXidsValid",
                            mul_size(sizeof(bool),
                                procArray->maxKnownAssignedXids),
                            &found);
    }

    /* Register and initialize fields of ProcLWLockTranche */
    LWLockRegisterTranche(LWTRANCHE_PROC, "proc");
#ifdef __TBASE__
    LWLockRegisterTranche(LWTRANCHE_PROC_DATA, "proc_data");
#endif
}

/*
 * Add the specified PGPROC to the shared array.
 */
void
ProcArrayAdd(PGPROC *proc)
{// #lizard forgives
    ProcArrayStruct *arrayP = procArray;
    int            index;

    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    if (arrayP->numProcs >= arrayP->maxProcs)
    {
        /*
         * Oops, no room.  (This really shouldn't happen, since there is a
         * fixed supply of PGPROC structs too, and so we should have failed
         * earlier.)
         */
        LWLockRelease(ProcArrayLock);
        ereport(FATAL,
                (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
                 errmsg("sorry, too many clients already")));
    }

    /*
     * Keep the procs array sorted by (PGPROC *) so that we can utilize
     * locality of references much better. This is useful while traversing the
     * ProcArray because there is an increased likelihood of finding the next
     * PGPROC structure in the cache.
     *
     * Since the occurrence of adding/removing a proc is much lower than the
     * access to the ProcArray itself, the overhead should be marginal
     */
    for (index = 0; index < arrayP->numProcs; index++)
    {
        /*
         * If we are the first PGPROC or if we have found our right position
         * in the array, break
         */
        if ((arrayP->pgprocnos[index] == -1) || (arrayP->pgprocnos[index] > proc->pgprocno))
            break;
    }

    memmove(&arrayP->pgprocnos[index + 1], &arrayP->pgprocnos[index],
            (arrayP->numProcs - index) * sizeof(int));
    arrayP->pgprocnos[index] = proc->pgprocno;
    arrayP->numProcs++;

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    if(IsConnFromCoord() && IS_PGXC_DATANODE)
    {
        /* 
         * As procarrary lock is held and only the local proc is accessed, 
         * per-proc lock is not necessary.
         */
        if(MyProc->hasGlobalXid)
        {
            strncpy(proc->globalXid, MyProc->globalXid, NAMEDATALEN);
            MyProc->hasGlobalXid = false;
            proc->hasGlobalXid = true;
            elog(DEBUG8, "myproc no %d transfer global xid %s procno %d", MyProc->pgprocno, proc->globalXid, proc->pgprocno);
        }
    }
#endif
    LWLockRelease(ProcArrayLock);
}

/*
 * Remove the specified PGPROC from the shared array.
 *
 * When latestXid is a valid XID, we are removing a live 2PC gxact from the
 * array, and thus causing it to appear as "not running" anymore.  In this
 * case we must advance latestCompletedXid.  (This is essentially the same
 * as ProcArrayEndTransaction followed by removal of the PGPROC, but we take
 * the ProcArrayLock only once, and don't damage the content of the PGPROC;
 * twophase.c depends on the latter.)
 */
void
ProcArrayRemove(PGPROC *proc, TransactionId latestXid)
{// #lizard forgives
    ProcArrayStruct *arrayP = procArray;
    int            index;

#ifdef XIDCACHE_DEBUG
    /* dump stats at backend shutdown, but not prepared-xact end */
    if (proc->pid != 0)
        DisplayXidCache();
#endif

    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    /* 
     * As procarrary lock is held and only the local proc is accessed, 
     * per-proc lock is not necessary.
     */
    elog(DEBUG8, "remove global xid %s %d", proc->globalXid, proc->pgprocno);
    proc->hasGlobalXid = false;
#endif


    if (TransactionIdIsValid(latestXid))
    {
        Assert(TransactionIdIsValid(allPgXact[proc->pgprocno].xid));

        /* Advance global latestCompletedXid while holding the lock */
        if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid,
                                  latestXid))
            ShmemVariableCache->latestCompletedXid = latestXid;

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
        LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
        if(MyProc->commitTs > ShmemVariableCache->latestCommitTs)
        {
            ShmemVariableCache->latestCommitTs = MyProc->commitTs;
            
            if(enable_distri_print)
            {
                elog(LOG, "latest committs xid %d ts " INT64_FORMAT, latestXid, MyProc->commitTs);
            }
            MyProc->commitTs = InvalidGlobalTimestamp;
        }

        if(MyProc->commitTs > ShmemVariableCache->latestGTS)
        {
            ShmemVariableCache->latestGTS = MyProc->commitTs;
        }        
        LWLockRelease(CommitTsLock);
#endif
    }
    else
    {
#ifdef PGXC
        if (IS_PGXC_DATANODE || !IsConnFromCoord())
#endif
        /* Shouldn't be trying to remove a live transaction here */
        Assert(!TransactionIdIsValid(allPgXact[proc->pgprocno].xid));
    }

    for (index = 0; index < arrayP->numProcs; index++)
    {
        if (arrayP->pgprocnos[index] == proc->pgprocno)
        {
            /* Keep the PGPROC array sorted. See notes above */
            memmove(&arrayP->pgprocnos[index], &arrayP->pgprocnos[index + 1],
                    (arrayP->numProcs - index - 1) * sizeof(int));
            arrayP->pgprocnos[arrayP->numProcs - 1] = -1;    /* for debugging */
            arrayP->numProcs--;
            LWLockRelease(ProcArrayLock);
            return;
        }
    }

    /* Oops */
    LWLockRelease(ProcArrayLock);

    elog(LOG, "failed to find proc %p in ProcArray", proc);
}


/*
 * ProcArrayEndTransaction -- mark a transaction as no longer running
 *
 * This is used interchangeably for commit and abort cases.  The transaction
 * commit/abort must already be reported to WAL and pg_xact.
 *
 * proc is currently always MyProc, but we pass it explicitly for flexibility.
 * latestXid is the latest Xid among the transaction's main XID and
 * subtransactions, or InvalidTransactionId if it has no XID.  (We must ask
 * the caller to pass latestXid, instead of computing it from the PGPROC's
 * contents, because the subxid information in the PGPROC might be
 * incomplete.)
 */
void
ProcArrayEndTransaction(PGPROC *proc, TransactionId latestXid)
{
    PGXACT       *pgxact = &allPgXact[proc->pgprocno];

    if (TransactionIdIsValid(latestXid))
    {
        /*
         * We must lock ProcArrayLock while clearing our advertised XID, so
         * that we do not exit the set of "running" transactions while someone
         * else is taking a snapshot.  See discussion in
         * src/backend/access/transam/README.
         */
#ifdef PGXC
        /*
         * Remove this assertion. We have seen this failing because a ROLLBACK
         * statement may get canceled by a Coordinator, leading to recursive
         * abort of a transaction. This must be a PostgreSQL issue, highlighted
         * by XC. See thread on hackers with subject "Canceling ROLLBACK
         * statement"
         */
#else
        Assert(TransactionIdIsValid(allPgXact[proc->pgprocno].xid));
#endif

        /*
         * If we can immediately acquire ProcArrayLock, we clear our own XID
         * and release the lock.  If not, use group XID clearing to improve
         * efficiency.
         */
        if (LWLockConditionalAcquire(ProcArrayLock, LW_EXCLUSIVE))
        {
            ProcArrayEndTransactionInternal(proc, pgxact, latestXid);
            LWLockRelease(ProcArrayLock);
        }
        else
            ProcArrayGroupClearXid(proc, latestXid);
    }
    else
    {
        /*
         * If we have no XID, we don't need to lock, since we won't affect
         * anyone else's calculation of a snapshot.  We might change their
         * estimate of global xmin, but that's OK.
         */
#ifdef XCP
        if (IsConnFromDatanode())
            allPgXact[proc->pgprocno].xid = InvalidTransactionId;
#endif
        Assert(!TransactionIdIsValid(allPgXact[proc->pgprocno].xid));

        proc->lxid = InvalidLocalTransactionId;
        pgxact->xmin = InvalidTransactionId;
        /* must be cleared with xid/xmin: */
        pgxact->vacuumFlags &= ~PROC_VACUUM_STATE_MASK;
        pgxact->delayChkpt = false; /* be sure this is cleared in abort */
        proc->recoveryConflictPending = false;

#ifdef    __TBASE__
        /* Clear the subtransaction-XID cache too while holding the lock */
        pgxact->nxids = 0;
        pgxact->overflowed = false;
#endif

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
        elog(DEBUG8, "invalid pgxact tmin");
        pg_atomic_write_u64(&pgxact->tmin, InvalidGlobalTimestamp);
        pg_atomic_write_u64(&pgxact->prepare_timestamp, InvalidGlobalTimestamp);
#endif

        Assert(pgxact->nxids == 0);
        Assert(pgxact->overflowed == false);
    }
}

/*
 * Mark a write transaction as no longer running.
 *
 * We don't do any locking here; caller must handle that.
 */
static inline void
ProcArrayEndTransactionInternal(PGPROC *proc, PGXACT *pgxact,
                                TransactionId latestXid)
{

    pgxact->xid = InvalidTransactionId;
    proc->lxid = InvalidLocalTransactionId;
    pgxact->xmin = InvalidTransactionId;
    /* must be cleared with xid/xmin: */
    pgxact->vacuumFlags &= ~PROC_VACUUM_STATE_MASK;
    pgxact->delayChkpt = false; /* be sure this is cleared in abort */
    proc->recoveryConflictPending = false;

    /* Clear the subtransaction-XID cache too while holding the lock */
    pgxact->nxids = 0;
    pgxact->overflowed = false;

    /* Also advance global latestCompletedXid while holding the lock */
    if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid,
                              latestXid))
        ShmemVariableCache->latestCompletedXid = latestXid;

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    /* avoid concurrency conflicts with the checkpoint */

    LWLockAcquire(CommitTsLock, LW_EXCLUSIVE);
    if(proc->commitTs > ShmemVariableCache->latestCommitTs)
    {
        ShmemVariableCache->latestCommitTs = proc->commitTs;
        
        if(enable_distri_print)
        {
            elog(LOG, "latest committs xid %d ts " INT64_FORMAT, latestXid, proc->commitTs);
        }
        proc->commitTs = InvalidGlobalTimestamp;
    }

    if(MyProc->commitTs > ShmemVariableCache->latestGTS)
    {
        ShmemVariableCache->latestGTS = MyProc->commitTs;
    }
    LWLockRelease(CommitTsLock);
    
    elog(DEBUG8, "invalid pgxact tmin xid %d", latestXid);
    pg_atomic_write_u64(&pgxact->tmin, InvalidGlobalTimestamp);
    pg_atomic_write_u64(&pgxact->prepare_timestamp, InvalidGlobalTimestamp);
#endif

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    /* 
     * As procarrary lock is held and only the local proc is accessed, 
     * per-proc lock is not necessary.
     * On the condition of group clearing, as other back-ends are hung on the queue,
     * there is no concurrency.
     */
    elog(DEBUG8, "remove my proc global xid %s procno %d", proc->globalXid, proc->pgprocno);
    proc->hasGlobalXid = false;
#endif

}

/*
 * ProcArrayGroupClearXid -- group XID clearing
 *
 * When we cannot immediately acquire ProcArrayLock in exclusive mode at
 * commit time, add ourselves to a list of processes that need their XIDs
 * cleared.  The first process to add itself to the list will acquire
 * ProcArrayLock in exclusive mode and perform ProcArrayEndTransactionInternal
 * on behalf of all group members.  This avoids a great deal of contention
 * around ProcArrayLock when many processes are trying to commit at once,
 * since the lock need not be repeatedly handed off from one committing
 * process to the next.
 */
static void
ProcArrayGroupClearXid(PGPROC *proc, TransactionId latestXid)
{// #lizard forgives
    volatile PROC_HDR *procglobal = ProcGlobal;
    uint32        nextidx;
    uint32        wakeidx;

    /* We should definitely have an XID to clear. */
    Assert(TransactionIdIsValid(allPgXact[proc->pgprocno].xid));

    /* Add ourselves to the list of processes needing a group XID clear. */
    proc->procArrayGroupMember = true;
    proc->procArrayGroupMemberXid = latestXid;
    while (true)
    {
        nextidx = pg_atomic_read_u32(&procglobal->procArrayGroupFirst);
        pg_atomic_write_u32(&proc->procArrayGroupNext, nextidx);

        if (pg_atomic_compare_exchange_u32(&procglobal->procArrayGroupFirst,
                                           &nextidx,
                                           (uint32) proc->pgprocno))
            break;
    }

    /*
     * If the list was not empty, the leader will clear our XID.  It is
     * impossible to have followers without a leader because the first process
     * that has added itself to the list will always have nextidx as
     * INVALID_PGPROCNO.
     */
    if (nextidx != INVALID_PGPROCNO)
    {
        int            extraWaits = 0;

        /* Sleep until the leader clears our XID. */
        pgstat_report_wait_start(WAIT_EVENT_PROCARRAY_GROUP_UPDATE);
        for (;;)
        {
            /* acts as a read barrier */
            PGSemaphoreLock(proc->sem);
            if (!proc->procArrayGroupMember)
                break;
            extraWaits++;
        }
        pgstat_report_wait_end();

        Assert(pg_atomic_read_u32(&proc->procArrayGroupNext) == INVALID_PGPROCNO);

        /* Fix semaphore count for any absorbed wakeups */
        while (extraWaits-- > 0)
            PGSemaphoreUnlock(proc->sem);
        return;
    }

    /* We are the leader.  Acquire the lock on behalf of everyone. */
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    /*
     * Now that we've got the lock, clear the list of processes waiting for
     * group XID clearing, saving a pointer to the head of the list.  Trying
     * to pop elements one at a time could lead to an ABA problem.
     */
    while (true)
    {
        nextidx = pg_atomic_read_u32(&procglobal->procArrayGroupFirst);
        if (pg_atomic_compare_exchange_u32(&procglobal->procArrayGroupFirst,
                                           &nextidx,
                                           INVALID_PGPROCNO))
            break;
    }

    /* Remember head of list so we can perform wakeups after dropping lock. */
    wakeidx = nextidx;

    /* Walk the list and clear all XIDs. */
    while (nextidx != INVALID_PGPROCNO)
    {
        PGPROC       *proc = &allProcs[nextidx];
        PGXACT       *pgxact = &allPgXact[nextidx];

        ProcArrayEndTransactionInternal(proc, pgxact, proc->procArrayGroupMemberXid);

        /* Move to next proc in list. */
        nextidx = pg_atomic_read_u32(&proc->procArrayGroupNext);
    }

    /* We're done with the lock now. */
    LWLockRelease(ProcArrayLock);

    /*
     * Now that we've released the lock, go back and wake everybody up.  We
     * don't do this under the lock so as to keep lock hold times to a
     * minimum.  The system calls we need to perform to wake other processes
     * up are probably much slower than the simple memory writes we did while
     * holding the lock.
     */
    while (wakeidx != INVALID_PGPROCNO)
    {
        PGPROC       *proc = &allProcs[wakeidx];

        wakeidx = pg_atomic_read_u32(&proc->procArrayGroupNext);
        pg_atomic_write_u32(&proc->procArrayGroupNext, INVALID_PGPROCNO);

        /* ensure all previous writes are visible before follower continues. */
        pg_write_barrier();

        proc->procArrayGroupMember = false;

        if (proc != MyProc)
            PGSemaphoreUnlock(proc->sem);
    }
}

/*
 * ProcArrayClearTransaction -- clear the transaction fields
 *
 * This is used after successfully preparing a 2-phase transaction.  We are
 * not actually reporting the transaction's XID as no longer running --- it
 * will still appear as running because the 2PC's gxact is in the ProcArray
 * too.  We just have to clear out our own PGXACT.
 */
void
ProcArrayClearTransaction(PGPROC *proc)
{
    PGXACT       *pgxact = &allPgXact[proc->pgprocno];

    /*
     * We can skip locking ProcArrayLock here, because this action does not
     * actually change anyone's view of the set of running XIDs: our entry is
     * duplicate with the gxact that has already been inserted into the
     * ProcArray.
     */
    pgxact->xid = InvalidTransactionId;
    proc->lxid = InvalidLocalTransactionId;
    pgxact->xmin = InvalidTransactionId;
    proc->recoveryConflictPending = false;

    /* redundant, but just in case */
    pgxact->vacuumFlags &= ~PROC_VACUUM_STATE_MASK;
    pgxact->delayChkpt = false;

    /* Clear the subtransaction-XID cache too */
    pgxact->nxids = 0;
    pgxact->overflowed = false;

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    pg_atomic_write_u64(&pgxact->tmin, InvalidGlobalTimestamp);
    pg_atomic_write_u64(&pgxact->prepare_timestamp, InvalidGlobalTimestamp);
#endif
}

/*
 * ProcArrayInitRecovery -- initialize recovery xid mgmt environment
 *
 * Remember up to where the startup process initialized the CLOG and subtrans
 * so we can ensure it's initialized gaplessly up to the point where necessary
 * while in recovery.
 */
void
ProcArrayInitRecovery(TransactionId initializedUptoXID)
{
    Assert(standbyState == STANDBY_INITIALIZED);
    Assert(TransactionIdIsNormal(initializedUptoXID));

    /*
     * we set latestObservedXid to the xid SUBTRANS has been initialized up
     * to, so we can extend it from that point onwards in
     * RecordKnownAssignedTransactionIds, and when we get consistent in
     * ProcArrayApplyRecoveryInfo().
     */
    latestObservedXid = initializedUptoXID;
    TransactionIdRetreat(latestObservedXid);
}

/*
 * ProcArrayApplyRecoveryInfo -- apply recovery info about xids
 *
 * Takes us through 3 states: Initialized, Pending and Ready.
 * Normal case is to go all the way to Ready straight away, though there
 * are atypical cases where we need to take it in steps.
 *
 * Use the data about running transactions on master to create the initial
 * state of KnownAssignedXids. We also use these records to regularly prune
 * KnownAssignedXids because we know it is possible that some transactions
 * with FATAL errors fail to write abort records, which could cause eventual
 * overflow.
 *
 * See comments for LogStandbySnapshot().
 */
void
ProcArrayApplyRecoveryInfo(RunningTransactions running)
{// #lizard forgives
    TransactionId *xids;
    int            nxids;
    TransactionId nextXid;
    int            i;

    Assert(standbyState >= STANDBY_INITIALIZED);
    Assert(TransactionIdIsValid(running->nextXid));
    Assert(TransactionIdIsValid(running->oldestRunningXid));
    Assert(TransactionIdIsNormal(running->latestCompletedXid));

    /*
     * Remove stale transactions, if any.
     */
    ExpireOldKnownAssignedTransactionIds(running->oldestRunningXid);

    /*
     * Remove stale locks, if any.
     *
     * Locks are always assigned to the toplevel xid so we don't need to care
     * about subxcnt/subxids (and by extension not about ->suboverflowed).
     */
    StandbyReleaseOldLocks(running->xcnt, running->xids);

    /*
     * If our snapshot is already valid, nothing else to do...
     */
    if (standbyState == STANDBY_SNAPSHOT_READY)
        return;

    /*
     * If our initial RunningTransactionsData had an overflowed snapshot then
     * we knew we were missing some subxids from our snapshot. If we continue
     * to see overflowed snapshots then we might never be able to start up, so
     * we make another test to see if our snapshot is now valid. We know that
     * the missing subxids are equal to or earlier than nextXid. After we
     * initialise we continue to apply changes during recovery, so once the
     * oldestRunningXid is later than the nextXid from the initial snapshot we
     * know that we no longer have missing information and can mark the
     * snapshot as valid.
     */
    if (standbyState == STANDBY_SNAPSHOT_PENDING)
    {
        /*
         * If the snapshot isn't overflowed or if its empty we can reset our
         * pending state and use this snapshot instead.
         */
        if (!running->subxid_overflow || running->xcnt == 0)
        {
            /*
             * If we have already collected known assigned xids, we need to
             * throw them away before we apply the recovery snapshot.
             */
            KnownAssignedXidsReset();
            standbyState = STANDBY_INITIALIZED;
        }
        else
        {
            if (TransactionIdPrecedes(standbySnapshotPendingXmin,
                                      running->oldestRunningXid))
            {
                standbyState = STANDBY_SNAPSHOT_READY;
                elog(trace_recovery(DEBUG1),
                     "recovery snapshots are now enabled");
            }
            else
                elog(trace_recovery(DEBUG1),
                     "recovery snapshot waiting for non-overflowed snapshot or "
                     "until oldest active xid on standby is at least %u (now %u)",
                     standbySnapshotPendingXmin,
                     running->oldestRunningXid);
            return;
        }
    }

    Assert(standbyState == STANDBY_INITIALIZED);

    /*
     * OK, we need to initialise from the RunningTransactionsData record.
     *
     * NB: this can be reached at least twice, so make sure new code can deal
     * with that.
     */

    /*
     * Nobody else is running yet, but take locks anyhow
     */
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    /*
     * KnownAssignedXids is sorted so we cannot just add the xids, we have to
     * sort them first.
     *
     * Some of the new xids are top-level xids and some are subtransactions.
     * We don't call SubtransSetParent because it doesn't matter yet. If we
     * aren't overflowed then all xids will fit in snapshot and so we don't
     * need subtrans. If we later overflow, an xid assignment record will add
     * xids to subtrans. If RunningXacts is overflowed then we don't have
     * enough information to correctly update subtrans anyway.
     */

    /*
     * Allocate a temporary array to avoid modifying the array passed as
     * argument.
     */
    xids = palloc(sizeof(TransactionId) * (running->xcnt + running->subxcnt));

    /*
     * Add to the temp array any xids which have not already completed.
     */
    nxids = 0;
    for (i = 0; i < running->xcnt + running->subxcnt; i++)
    {
        TransactionId xid = running->xids[i];

        /*
         * The running-xacts snapshot can contain xids that were still visible
         * in the procarray when the snapshot was taken, but were already
         * WAL-logged as completed. They're not running anymore, so ignore
         * them.
         */
        if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
            continue;

        xids[nxids++] = xid;
    }

    if (nxids > 0)
    {
        if (procArray->numKnownAssignedXids != 0)
        {
            LWLockRelease(ProcArrayLock);
            elog(ERROR, "KnownAssignedXids is not empty");
        }

        /*
         * Sort the array so that we can add them safely into
         * KnownAssignedXids.
         */
        qsort(xids, nxids, sizeof(TransactionId), xidComparator);

        /*
		 * Add the sorted snapshot into KnownAssignedXids.  The running-xacts
		 * snapshot may include duplicated xids because of prepared
		 * transactions, so ignore them.
         */
        for (i = 0; i < nxids; i++)
		{
			if (i > 0 && TransactionIdEquals(xids[i - 1], xids[i]))
			{
				elog(DEBUG1,
					 "found duplicated transaction %u for KnownAssignedXids insertion",
					 xids[i]);
				continue;
			}
            KnownAssignedXidsAdd(xids[i], xids[i], true);
		}

        KnownAssignedXidsDisplay(trace_recovery(DEBUG3));
    }

    pfree(xids);

    /*
     * latestObservedXid is at least set to the point where SUBTRANS was
     * started up to (c.f. ProcArrayInitRecovery()) or to the biggest xid
     * RecordKnownAssignedTransactionIds() was called for.  Initialize
     * subtrans from thereon, up to nextXid - 1.
     *
     * We need to duplicate parts of RecordKnownAssignedTransactionId() here,
     * because we've just added xids to the known assigned xids machinery that
     * haven't gone through RecordKnownAssignedTransactionId().
     */
    Assert(TransactionIdIsNormal(latestObservedXid));
    TransactionIdAdvance(latestObservedXid);
    while (TransactionIdPrecedes(latestObservedXid, running->nextXid))
    {
        ExtendSUBTRANS(latestObservedXid);
        TransactionIdAdvance(latestObservedXid);
    }
    TransactionIdRetreat(latestObservedXid);    /* = running->nextXid - 1 */

    /* ----------
     * Now we've got the running xids we need to set the global values that
     * are used to track snapshots as they evolve further.
     *
     * - latestCompletedXid which will be the xmax for snapshots
     * - lastOverflowedXid which shows whether snapshots overflow
     * - nextXid
     *
     * If the snapshot overflowed, then we still initialise with what we know,
     * but the recovery snapshot isn't fully valid yet because we know there
     * are some subxids missing. We don't know the specific subxids that are
     * missing, so conservatively assume the last one is latestObservedXid.
     * ----------
     */
    if (running->subxid_overflow)
    {
        standbyState = STANDBY_SNAPSHOT_PENDING;

        standbySnapshotPendingXmin = latestObservedXid;
        procArray->lastOverflowedXid = latestObservedXid;
    }
    else
    {
        standbyState = STANDBY_SNAPSHOT_READY;

        standbySnapshotPendingXmin = InvalidTransactionId;
    }

    /*
     * If a transaction wrote a commit record in the gap between taking and
     * logging the snapshot then latestCompletedXid may already be higher than
     * the value from the snapshot, so check before we use the incoming value.
     */
    if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid,
                              running->latestCompletedXid))
        ShmemVariableCache->latestCompletedXid = running->latestCompletedXid;

    Assert(TransactionIdIsNormal(ShmemVariableCache->latestCompletedXid));

    LWLockRelease(ProcArrayLock);

    /*
     * ShmemVariableCache->nextXid must be beyond any observed xid.
     *
     * We don't expect anyone else to modify nextXid, hence we don't need to
     * hold a lock while examining it.  We still acquire the lock to modify
     * it, though.
     */
    nextXid = latestObservedXid;
    TransactionIdAdvance(nextXid);
    if (TransactionIdFollows(nextXid, ShmemVariableCache->nextXid))
    {
        LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
        ShmemVariableCache->nextXid = nextXid;
        LWLockRelease(XidGenLock);
    }

    Assert(TransactionIdIsValid(ShmemVariableCache->nextXid));

    KnownAssignedXidsDisplay(trace_recovery(DEBUG3));
    if (standbyState == STANDBY_SNAPSHOT_READY)
        elog(trace_recovery(DEBUG1), "recovery snapshots are now enabled");
    else
        elog(trace_recovery(DEBUG1),
             "recovery snapshot waiting for non-overflowed snapshot or "
             "until oldest active xid on standby is at least %u (now %u)",
             standbySnapshotPendingXmin,
             running->oldestRunningXid);
}

/*
 * ProcArrayApplyXidAssignment
 *        Process an XLOG_XACT_ASSIGNMENT WAL record
 */
void
ProcArrayApplyXidAssignment(TransactionId topxid,
                            int nsubxids, TransactionId *subxids)
{
    TransactionId max_xid;
    int            i;

    Assert(standbyState >= STANDBY_INITIALIZED);

    max_xid = TransactionIdLatest(topxid, nsubxids, subxids);

    /*
     * Mark all the subtransactions as observed.
     *
     * NOTE: This will fail if the subxid contains too many previously
     * unobserved xids to fit into known-assigned-xids. That shouldn't happen
     * as the code stands, because xid-assignment records should never contain
     * more than PGPROC_MAX_CACHED_SUBXIDS entries.
     */
    RecordKnownAssignedTransactionIds(max_xid);

    /*
     * Notice that we update pg_subtrans with the top-level xid, rather than
     * the parent xid. This is a difference between normal processing and
     * recovery, yet is still correct in all cases. The reason is that
     * subtransaction commit is not marked in clog until commit processing, so
     * all aborted subtransactions have already been clearly marked in clog.
     * As a result we are able to refer directly to the top-level
     * transaction's state rather than skipping through all the intermediate
     * states in the subtransaction tree. This should be the first time we
     * have attempted to SubTransSetParent().
     */
    for (i = 0; i < nsubxids; i++)
        SubTransSetParent(subxids[i], topxid);

    /* KnownAssignedXids isn't maintained yet, so we're done for now */
    if (standbyState == STANDBY_INITIALIZED)
        return;

    /*
     * Uses same locking as transaction commit
     */
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    /*
     * Remove subxids from known-assigned-xacts.
     */
    KnownAssignedXidsRemoveTree(InvalidTransactionId, nsubxids, subxids);

    /*
     * Advance lastOverflowedXid to be at least the last of these subxids.
     */
    if (TransactionIdPrecedes(procArray->lastOverflowedXid, max_xid))
        procArray->lastOverflowedXid = max_xid;

    LWLockRelease(ProcArrayLock);
}


#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
/*
 * In our distributed transaction concurrency protocol, 
 * we should wait for the transaction to complete if it has been prepared.
 * Return true if the xid has been prepared, committed or aborted.
 * Otherwise, return false.
 */

bool
TransactionIdIsPrepared(TransactionId xid, Snapshot snapshot, GlobalTimestamp *prepare_ts)
{// #lizard forgives
    static TransactionId *xids = NULL;
    static GlobalTimestamp *prepare_timestamp = NULL;
    int            nxids = 0;
    ProcArrayStruct *arrayP = procArray;
    TransactionId topxid;
    int            i,
                j;
    
    *prepare_ts = InvalidGlobalTimestamp;

    /*
     * Don't bother checking a transaction older than RecentXmin; it could not
     * possibly still be running.  (Note: in particular, this guarantees that
     * we reject InvalidTransactionId, FrozenTransactionId, etc as not
     * running.)
     */
    if (TransactionIdPrecedes(xid, RecentXmin))
    {
        xc_by_recent_xmin_inc();
        return true;
    }


    /*
     * Also, we can handle our own transaction (and subtransactions) without
     * any access to shared memory.
     */
    if (TransactionIdIsCurrentTransactionId(xid))
    {
        xc_by_my_xact_inc();
        ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("transaction %d should not wait for itsef to complete. autovacuum %d", 
                         xid, IsAutoVacuumWorkerProcess())));
    }

    /*
     * If first time through, get workspace to remember main XIDs in. We
     * malloc it permanently to avoid repeated palloc/pfree overhead.
     */
    if (xids == NULL)
    {
        /*
         * In hot standby mode, reserve enough space to hold all xids in the
         * known-assigned list. If we later finish recovery, we no longer need
         * the bigger array, but we don't bother to shrink it.
         */
        int            maxxids = RecoveryInProgress() ? TOTAL_MAX_CACHED_SUBXIDS : arrayP->maxProcs;

        xids = (TransactionId *) malloc(maxxids * sizeof(TransactionId));
        if (xids == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
        prepare_timestamp = (GlobalTimestamp *) malloc(maxxids * sizeof(GlobalTimestamp));
        if (prepare_timestamp == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));

    }

    LWLockAcquire(ProcArrayLock, LW_SHARED);


    /* No shortcuts, gotta grovel through the array */
    for (i = 0; i < arrayP->numProcs; i++)
    {
        int            pgprocno = arrayP->pgprocnos[i];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        TransactionId pxid;

        /* Ignore my own proc --- dealt with it above */
        if (proc == MyProc)
            continue;

        /* Fetch xid just once - see GetNewTransactionId */
        pxid = pgxact->xid;

        if (!TransactionIdIsValid(pxid))
            continue;

        /*
         * Step 1: check the main Xid
         */
        if (TransactionIdEquals(pxid, xid))
        {
            bool res;
                
            *prepare_ts  = pg_atomic_read_u64(&pgxact->prepare_timestamp);
            res = GlobalTimestampIsValid(*prepare_ts) ? true:false;

            LWLockRelease(ProcArrayLock);
            xc_by_main_xid_inc();
            return res;
            
        }

        /*
         * We can ignore main Xids that are younger than the target Xid, since
         * the target could not possibly be their child.
         */
        if (TransactionIdPrecedes(xid, pxid))
            continue;

        /*
         * Step 2: check the cached child-Xids arrays
         */
        for (j = pgxact->nxids - 1; j >= 0; j--)
        {
            /* Fetch xid just once - see GetNewTransactionId */
            TransactionId cxid = proc->subxids.xids[j];

            if (TransactionIdEquals(cxid, xid))
            {
                bool res;
                
                *prepare_ts  = pg_atomic_read_u64(&pgxact->prepare_timestamp);
                res = GlobalTimestampIsValid(*prepare_ts) ? true:false;
                
                LWLockRelease(ProcArrayLock);
                xc_by_child_xid_inc();
                return res;
            }
        }

        /*
         * Save the main Xid for step 4.  We only need to remember main Xids
         * that have uncached children.  (Note: there is no race condition
         * here because the overflowed flag cannot be cleared, only set, while
         * we hold ProcArrayLock.  So we can't miss an Xid that we need to
         * worry about.)
         */
        if (pgxact->overflowed)
        {
            xids[nxids] = pxid;
            prepare_timestamp[nxids++] = pg_atomic_read_u64(&pgxact->prepare_timestamp);
        }
    }


    LWLockRelease(ProcArrayLock);


    /*
     * Step 4: have to check pg_subtrans.
     *
     * At this point, we know it's either a subtransaction of one of the Xids
     * in xids[], or it's not running.  If it's an already-failed
     * subtransaction, we want to say "not running" even though its parent may
     * still be running.  So first, check pg_xact to see if it's been aborted.
     */
    xc_slow_answer_inc();

    if (TransactionIdDidAbort(xid))
        return true;

    /*
     * It isn't aborted, so check whether the transaction tree it belongs to
     * is still running (or, more precisely, whether it was running when we
     * held ProcArrayLock).
     */
    topxid = SubTransGetTopmostTransaction(xid);
    Assert(TransactionIdIsValid(topxid));
    if (!TransactionIdEquals(topxid, xid))
    {
        for (i = 0; i < nxids; i++)
        {
            if (TransactionIdEquals(xids[i], topxid))
            {
                if(GlobalTimestampIsValid(prepare_timestamp[i]))
                {
                    *prepare_ts = prepare_timestamp[i];
                    return true;
                }
                else
                    return false;
            }
        }
    }

    return true;
}

#endif
/*
 * TransactionIdIsInProgress -- is given transaction running in some backend
 *
 * Aside from some shortcuts such as checking RecentXmin and our own Xid,
 * there are four possibilities for finding a running transaction:
 *
 * 1. The given Xid is a main transaction Id.  We will find this out cheaply
 * by looking at the PGXACT struct for each backend.
 *
 * 2. The given Xid is one of the cached subxact Xids in the PGPROC array.
 * We can find this out cheaply too.
 *
 * 3. In Hot Standby mode, we must search the KnownAssignedXids list to see
 * if the Xid is running on the master.
 *
 * 4. Search the SubTrans tree to find the Xid's topmost parent, and then see
 * if that is running according to PGXACT or KnownAssignedXids.  This is the
 * slowest way, but sadly it has to be done always if the others failed,
 * unless we see that the cached subxact sets are complete (none have
 * overflowed).
 *
 * ProcArrayLock has to be held while we do 1, 2, 3.  If we save the top Xids
 * while doing 1 and 3, we can release the ProcArrayLock while we do 4.
 * This buys back some concurrency (and we can't retrieve the main Xids from
 * PGXACT again anyway; see GetNewTransactionId).
 */
bool
TransactionIdIsInProgress(TransactionId xid)
{// #lizard forgives
    static TransactionId *xids = NULL;
    int            nxids = 0;
    ProcArrayStruct *arrayP = procArray;
    TransactionId topxid;
    int            i,
                j;

    /*
     * Don't bother checking a transaction older than RecentXmin; it could not
     * possibly still be running.  (Note: in particular, this guarantees that
     * we reject InvalidTransactionId, FrozenTransactionId, etc as not
     * running.)
     */
    if (TransactionIdPrecedes(xid, RecentXmin))
    {
        xc_by_recent_xmin_inc();
        return false;
    }

    /*
     * We may have just checked the status of this transaction, so if it is
     * already known to be completed, we can fall out without any access to
     * shared memory.
     */
    if (TransactionIdIsKnownCompleted(xid))
    {
        xc_by_known_xact_inc();
        return false;
    }
    /*
     * We should check clog status here as now one tuple becomes visible immediately
     * after its commit timestamps is written to committs log.
     * If TransactionIdIsInProgress performs just before tuple's transaction 
     * proc entry is removed from procarray, then the tuple may be regarded as invisible by mistaken.
     * Since the clog status is updated before writting committs log upon commits 
     * (See RecordTransactionCommit, RecordTransactionCommitPrepared),
     * the clog rechecking could avoid the above situation.
     */
    if(TransactionIdDidCommit(xid))
        return false;

    /*
     * Also, we can handle our own transaction (and subtransactions) without
     * any access to shared memory.
     */
    if (TransactionIdIsCurrentTransactionId(xid))
    {
        xc_by_my_xact_inc();
        return true;
    }

    /*
     * If first time through, get workspace to remember main XIDs in. We
     * malloc it permanently to avoid repeated palloc/pfree overhead.
     */
    if (xids == NULL)
    {
        /*
         * In hot standby mode, reserve enough space to hold all xids in the
         * known-assigned list. If we later finish recovery, we no longer need
         * the bigger array, but we don't bother to shrink it.
         */
        int            maxxids = RecoveryInProgress() ? TOTAL_MAX_CACHED_SUBXIDS : arrayP->maxProcs;

        xids = (TransactionId *) malloc(maxxids * sizeof(TransactionId));
        if (xids == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
    }

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    /*
     * Now that we have the lock, we can check latestCompletedXid; if the
     * target Xid is after that, it's surely still running.
     */
    if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid, xid))
    {
        LWLockRelease(ProcArrayLock);
        xc_by_latest_xid_inc();
        return true;
    }

    /* No shortcuts, gotta grovel through the array */
    for (i = 0; i < arrayP->numProcs; i++)
    {
        int            pgprocno = arrayP->pgprocnos[i];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        TransactionId pxid;

        /* Ignore my own proc --- dealt with it above */
        if (proc == MyProc)
            continue;

        /* Fetch xid just once - see GetNewTransactionId */
        pxid = pgxact->xid;

        if (!TransactionIdIsValid(pxid))
            continue;

        /*
         * Step 1: check the main Xid
         */
        if (TransactionIdEquals(pxid, xid))
        {
            LWLockRelease(ProcArrayLock);
            xc_by_main_xid_inc();
            return true;
        }

        /*
         * We can ignore main Xids that are younger than the target Xid, since
         * the target could not possibly be their child.
         */
        if (TransactionIdPrecedes(xid, pxid))
            continue;

        /*
         * Step 2: check the cached child-Xids arrays
         */
        for (j = pgxact->nxids - 1; j >= 0; j--)
        {
            /* Fetch xid just once - see GetNewTransactionId */
            TransactionId cxid = proc->subxids.xids[j];

            if (TransactionIdEquals(cxid, xid))
            {
                LWLockRelease(ProcArrayLock);
                xc_by_child_xid_inc();
                return true;
            }
        }

        /*
         * Save the main Xid for step 4.  We only need to remember main Xids
         * that have uncached children.  (Note: there is no race condition
         * here because the overflowed flag cannot be cleared, only set, while
         * we hold ProcArrayLock.  So we can't miss an Xid that we need to
         * worry about.)
         */
        if (pgxact->overflowed)
            xids[nxids++] = pxid;
    }

    /*
     * Step 3: in hot standby mode, check the known-assigned-xids list.  XIDs
     * in the list must be treated as running.
     */
    if (RecoveryInProgress())
    {
        /* none of the PGXACT entries should have XIDs in hot standby mode */
        Assert(nxids == 0);

        if (KnownAssignedXidExists(xid))
        {
            LWLockRelease(ProcArrayLock);
            xc_by_known_assigned_inc();
            return true;
        }

        /*
         * If the KnownAssignedXids overflowed, we have to check pg_subtrans
         * too.  Fetch all xids from KnownAssignedXids that are lower than
         * xid, since if xid is a subtransaction its parent will always have a
         * lower value.  Note we will collect both main and subXIDs here, but
         * there's no help for it.
         */
        if (TransactionIdPrecedesOrEquals(xid, procArray->lastOverflowedXid))
            nxids = KnownAssignedXidsGet(xids, xid);
    }

    LWLockRelease(ProcArrayLock);

    /*
     * If none of the relevant caches overflowed, we know the Xid is not
     * running without even looking at pg_subtrans.
     */
    if (nxids == 0)
    {
        xc_no_overflow_inc();
        return false;
    }

    /*
     * Step 4: have to check pg_subtrans.
     *
     * At this point, we know it's either a subtransaction of one of the Xids
     * in xids[], or it's not running.  If it's an already-failed
     * subtransaction, we want to say "not running" even though its parent may
     * still be running.  So first, check pg_xact to see if it's been aborted.
     */
    xc_slow_answer_inc();

    if (TransactionIdDidAbort(xid))
        return false;

    /*
     * It isn't aborted, so check whether the transaction tree it belongs to
     * is still running (or, more precisely, whether it was running when we
     * held ProcArrayLock).
     */
    topxid = SubTransGetTopmostTransaction(xid);
    Assert(TransactionIdIsValid(topxid));
    if (!TransactionIdEquals(topxid, xid))
    {
        for (i = 0; i < nxids; i++)
        {
            if (TransactionIdEquals(xids[i], topxid))
                return true;
        }
    }

    return false;
}

/*
 * TransactionIdIsActive -- is xid the top-level XID of an active backend?
 *
 * This differs from TransactionIdIsInProgress in that it ignores prepared
 * transactions, as well as transactions running on the master if we're in
 * hot standby.  Also, we ignore subtransactions since that's not needed
 * for current uses.
 */
bool
TransactionIdIsActive(TransactionId xid)
{
    bool        result = false;
    ProcArrayStruct *arrayP = procArray;
    int            i;

    /*
     * Don't bother checking a transaction older than RecentXmin; it could not
     * possibly still be running.
     */
    if (TransactionIdPrecedes(xid, RecentXmin))
        return false;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (i = 0; i < arrayP->numProcs; i++)
    {
        int            pgprocno = arrayP->pgprocnos[i];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        TransactionId pxid;

        /* Fetch xid just once - see GetNewTransactionId */
        pxid = pgxact->xid;

        if (!TransactionIdIsValid(pxid))
            continue;

        if (proc->pid == 0)
            continue;            /* ignore prepared transactions */

        if (TransactionIdEquals(pxid, xid))
        {
            result = true;
            break;
        }
    }

    LWLockRelease(ProcArrayLock);

    return result;
}


/*
 * GetOldestXmin -- returns oldest transaction that was running
 *                    when any current transaction was started.
 *
 * If rel is NULL or a shared relation, all backends are considered, otherwise
 * only backends running in this database are considered.
 *
 * The flags are used to ignore the backends in calculation when any of the
 * corresponding flags is set. Typically, if you want to ignore ones with
 * PROC_IN_VACUUM flag, you can use PROCARRAY_FLAGS_VACUUM.
 *
 * PROCARRAY_SLOTS_XMIN causes GetOldestXmin to ignore the xmin and
 * catalog_xmin of any replication slots that exist in the system when
 * calculating the oldest xmin.
 *
 * This is used by VACUUM to decide which deleted tuples must be preserved in
 * the passed in table. For shared relations backends in all databases must be
 * considered, but for non-shared relations that's not required, since only
 * backends in my own database could ever see the tuples in them. Also, we can
 * ignore concurrently running lazy VACUUMs because (a) they must be working
 * on other tables, and (b) they don't need to do snapshot-based lookups.
 *
 * This is also used to determine where to truncate pg_subtrans.  For that
 * backends in all databases have to be considered, so rel = NULL has to be
 * passed in.
 *
 * Note: we include all currently running xids in the set of considered xids.
 * This ensures that if a just-started xact has not yet set its snapshot,
 * when it does set the snapshot it cannot set xmin less than what we compute.
 * See notes in src/backend/access/transam/README.
 *
 * Note: despite the above, it's possible for the calculated value to move
 * backwards on repeated calls. The calculated value is conservative, so that
 * anything older is definitely not considered as running by anyone anymore,
 * but the exact value calculated depends on a number of things. For example,
 * if rel = NULL and there are no transactions running in the current
 * database, GetOldestXmin() returns latestCompletedXid. If a transaction
 * begins after that, its xmin will include in-progress transactions in other
 * databases that started earlier, so another call will return a lower value.
 * Nonetheless it is safe to vacuum a table in the current database with the
 * first result.  There are also replication-related effects: a walsender
 * process can set its xmin based on transactions that are no longer running
 * in the master but are still being replayed on the standby, thus possibly
 * making the GetOldestXmin reading go backwards.  In this case there is a
 * possibility that we lose data that the standby would like to have, but
 * unless the standby uses a replication slot to make its xmin persistent
 * there is little we can do about that --- data is only protected if the
 * walsender runs continuously while queries are executed on the standby.
 * (The Hot Standby code deals with such cases by failing standby queries
 * that needed to access already-removed data, so there's no integrity bug.)
 * The return value is also adjusted with vacuum_defer_cleanup_age, so
 * increasing that setting on the fly is another easy way to make
 * GetOldestXmin() move backwards, with no consequences for data integrity.
 */
TransactionId
GetOldestXmin(Relation rel, int flags)
{
    return GetOldestXminInternal(rel, flags, false,
            InvalidTransactionId);
}

/*
 * This implements most of the logic that GetOldestXmin needs. In XL, we don't
 * actually compute OldestXmin unless specifically told to do by computeLocal
 * argument set to true which GetOldestXmin never done. So we just return the
 * value from the shared memory. The OldestXmin itself is always computed by
 * the Cluster Monitor process by sending local state information to the GTM,
 * which then aggregates information from all the nodes and gives out final
 * OldestXmin or GlobalXmin which is consistent across the entire cluster.
 *
 * In addition, Cluster Monitor also passes the last reported xmin (or the one
 * sent back by GTM in case we were idle) and the last received GlobalXmin. We
 * must ensure that we don't see an XID or xmin which is beyond these horizons.
 * Otherwise it signals problems with the GlobalXmin calculation. This can
 * happen because of network disconnects or extreme load on the machine
 * (unlikely). In any case, we must restart ourselves to avoid any data
 * consistency problem. A more careful approach could involve killing only
 * those backends which are running with old xid or xmin. We can consider
 * implementing it that way in future
 */
TransactionId
GetOldestXminInternal(Relation rel, int flags, bool computeLocal,
        TransactionId lastGlobalXmin)
{// #lizard forgives
    ProcArrayStruct *arrayP = procArray;
    TransactionId result;
    int            index;
    bool        allDbs;
#ifdef __USE_GLOBAL_SNAPSHOT__
    TransactionId xmin;
#endif
    volatile TransactionId replication_slot_xmin = InvalidTransactionId;
    volatile TransactionId replication_slot_catalog_xmin = InvalidTransactionId;

    /*
     * If we're not computing a relation specific limit, or if a shared
     * relation has been passed in, backends in all databases have to be
     * considered.
     */
    allDbs = rel == NULL || rel->rd_rel->relisshared;

#ifdef __USE_GLOBAL_SNAPSHOT__
    if (!computeLocal)
    {
        xmin = (TransactionId) ClusterMonitorGetGlobalXmin();
        if (!TransactionIdIsValid(xmin))
            xmin = FirstNormalTransactionId;
        return xmin;
    }
#endif

    /* Cannot look for individual databases during recovery */
    Assert(allDbs || !RecoveryInProgress());

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    /*
     * We initialize the MIN() calculation with latestCompletedXid + 1. This
     * is a lower bound for the XIDs that might appear in the ProcArray later,
     * and so protects us against overestimating the result due to future
     * additions.
     */
    result = ShmemVariableCache->latestCompletedXid;
#ifdef __USE_GLOBAL_SNAPSHOT__
    if (!TransactionIdIsValid(result))
        result = FirstNormalTransactionId;
    else
        TransactionIdAdvance(result);

#else
    Assert(TransactionIdIsNormal(result));
    TransactionIdAdvance(result);
#endif

    elog(DEBUG1, "GetOldestXminInternal - Starting computation with"
            "latestCompletedXid %d + 1", result);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];

        if (pgxact->vacuumFlags & (flags & PROCARRAY_PROC_FLAGS_MASK))
            continue;

        if (allDbs ||
            proc->databaseId == MyDatabaseId ||
            proc->databaseId == 0)    /* always include WalSender */
        {
            /* Fetch xid just once - see GetNewTransactionId */
            TransactionId xid = pgxact->xid;
#ifdef __USE_GLOBAL_SNAPSHOT__
            TransactionId xmin = pgxact->xmin; /* Fetch just once */
#endif

            /* First consider the transaction's own Xid, if any */
            if (TransactionIdIsNormal(xid) &&
                TransactionIdPrecedes(xid, result))
                result = xid;

            /*
             * Also consider the transaction's Xmin, if set.
             *
             * We must check both Xid and Xmin because a transaction might
             * have an Xmin but not (yet) an Xid; conversely, if it has an
             * Xid, that could determine some not-yet-set Xmin.
             */
#ifdef __USE_GLOBAL_SNAPSHOT__

            elog(DEBUG1, "proc: pid:%d, xmin: %d, xid: %d", proc->pid,
                    xmin, xid);

            if (TransactionIdIsNormal(xmin) &&
                 TransactionIdPrecedes(xmin, result))
                result = xmin;

            /*
             * If we see an xid or an xmin which precedes the GlobalXmin calculated by the
             * Cluster Monitor process then it signals bad things and we must
             * abort and restart the database server
             */
            if (TransactionIdIsValid(lastGlobalXmin))
            {
                if ((TransactionIdIsValid(xmin) && TransactionIdPrecedes(xmin, lastGlobalXmin)) ||
                    (TransactionIdIsValid(xid) && TransactionIdPrecedes(xid,
                                                                        lastGlobalXmin)))
                    elog(PANIC, "Found xid (%d) or xmin (%d) precedes "
                            "global xmin (%d)", xid, xmin, lastGlobalXmin);
            }
#else
            xid = pgxact->xmin; /* Fetch just once */
            if (TransactionIdIsNormal(xid) &&
                TransactionIdPrecedes(xid, result))
                result = xid;
#endif
        }
    }

    /* fetch into volatile var while ProcArrayLock is held */
    replication_slot_xmin = procArray->replication_slot_xmin;
    replication_slot_catalog_xmin = procArray->replication_slot_catalog_xmin;

    if (RecoveryInProgress())
    {
        /*
         * Check to see whether KnownAssignedXids contains an xid value older
         * than the main procarray.
         */
        TransactionId kaxmin = KnownAssignedXidsGetOldestXmin();

        LWLockRelease(ProcArrayLock);

        if (TransactionIdIsNormal(kaxmin) &&
            TransactionIdPrecedes(kaxmin, result))
            result = kaxmin;
    }
    else
    {
        /*
         * No other information needed, so release the lock immediately.
         */
        LWLockRelease(ProcArrayLock);

        /*
         * Compute the cutoff XID by subtracting vacuum_defer_cleanup_age,
         * being careful not to generate a "permanent" XID.
         *
         * vacuum_defer_cleanup_age provides some additional "slop" for the
         * benefit of hot standby queries on standby servers.  This is quick
         * and dirty, and perhaps not all that useful unless the master has a
         * predictable transaction rate, but it offers some protection when
         * there's no walsender connection.  Note that we are assuming
         * vacuum_defer_cleanup_age isn't large enough to cause wraparound ---
         * so guc.c should limit it to no more than the xidStopLimit threshold
         * in varsup.c.  Also note that we intentionally don't apply
         * vacuum_defer_cleanup_age on standby servers.
         */
        result -= vacuum_defer_cleanup_age;
        if (!TransactionIdIsNormal(result))
            result = FirstNormalTransactionId;
    }

    /*
     * Check whether there are replication slots requiring an older xmin.
     */
    if (!(flags & PROCARRAY_SLOTS_XMIN) &&
        TransactionIdIsValid(replication_slot_xmin) &&
        NormalTransactionIdPrecedes(replication_slot_xmin, result))
        result = replication_slot_xmin;

    /*
     * After locks have been released and defer_cleanup_age has been applied,
     * check whether we need to back up further to make logical decoding
     * possible. We need to do so if we're computing the global limit (rel =
     * NULL) or if the passed relation is a catalog relation of some kind.
     */
    if (!(flags & PROCARRAY_SLOTS_XMIN) &&
        (rel == NULL ||
         RelationIsAccessibleInLogicalDecoding(rel)) &&
        TransactionIdIsValid(replication_slot_catalog_xmin) &&
        NormalTransactionIdPrecedes(replication_slot_catalog_xmin, result))
        result = replication_slot_catalog_xmin;

    return result;
}

/*
 * GetMaxSnapshotXidCount -- get max size for snapshot XID array
 *
 * We have to export this for use by snapmgr.c.
 */
int
GetMaxSnapshotXidCount(void)
{
    return procArray->maxProcs;
}

/*
 * GetMaxSnapshotSubxidCount -- get max size for snapshot sub-XID array
 *
 * We have to export this for use by snapmgr.c.
 */
int
GetMaxSnapshotSubxidCount(void)
{
    return TOTAL_MAX_CACHED_SUBXIDS;
}

#ifdef __TBASE__
TransactionId GetLocalTransactionId(const char *globalXid, TransactionId *subxids, int *nsub)
{
    
    ProcArrayStruct *arrayP = procArray;
    int           *pgprocnos;
    int            numProcs;
    int         index;

    LWLockAcquire(ProcArrayLock, LW_SHARED);
    numProcs = arrayP->numProcs;
    pgprocnos =  arrayP->pgprocnos;
    for (index = 0; index < numProcs; index++)
    {
        int         pgprocno = pgprocnos[index];
        PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
		TransactionId    result =  InvalidTransactionId;
		int         nxid;
        
        LWLockAcquire(&proc->globalxidLock, LW_SHARED);
        if (!proc->hasGlobalXid || strcmp(globalXid, proc->globalXid) != 0)
        {
            if(!proc->hasGlobalXid)
                elog(DEBUG8, "proc no %d null", proc->pgprocno);
            else
                elog(DEBUG8, "proc no %d global xid %s target %s", 
                proc->pgprocno, proc->globalXid, globalXid);
            LWLockRelease(&proc->globalxidLock);
            continue;
        }
        
		result = pgxact->xid;
		
		/* look for max xid in subtrans */
		*nsub = pgxact->nxids;
		for (nxid = 0; nxid < pgxact->nxids; nxid++)
		{
			TransactionId subxid = proc->subxids.xids[nxid];
			subxids[nxid] = subxid;
		}
		
        LWLockRelease(&proc->globalxidLock);
        LWLockRelease(ProcArrayLock);
		elog(DEBUG8, "found xid %d for global xid %s", result, globalXid);
		
		return result;
    }

    LWLockRelease(ProcArrayLock);
    return InvalidTransactionId;

}
#endif

char *GetGlobalTransactionId(const TransactionId pid)
{
    ProcArrayStruct *arrayP = procArray;
    int           *pgprocnos;
    int            numProcs;
    int         index;

    LWLockAcquire(ProcArrayLock, LW_SHARED);
    numProcs = arrayP->numProcs;
    pgprocnos =  arrayP->pgprocnos;
    for (index = 0; index < numProcs; index++)
    {
        int         pgprocno = pgprocnos[index];
        PGPROC *proc = &allProcs[pgprocno];
        
        LWLockAcquire(&proc->globalxidLock, LW_SHARED);
    
        if (!proc->hasGlobalXid || pid != proc->pid)
        {
            if (!proc->hasGlobalXid)
                elog(DEBUG8, "proc no %d null", proc->pgprocno);
            else
                elog(DEBUG8, "proc no %d pid %d target global xid %s", 
                proc->pgprocno, pid, proc->globalXid);
            LWLockRelease(&proc->globalxidLock);
            continue;
        }
        LWLockRelease(&proc->globalxidLock);
        LWLockRelease(ProcArrayLock);
        elog(DEBUG8, "found global xid %s for xid %d", proc->globalXid, pid);
        return proc->globalXid;
    }

    LWLockRelease(ProcArrayLock);
    return NULL;

}

/*
 * GetSnapshotData -- returns information about running transactions.
 *
 * The returned snapshot includes xmin (lowest still-running xact ID),
 * xmax (highest completed xact ID + 1), and a list of running xact IDs
 * in the range xmin <= xid < xmax.  It is used as follows:
 *        All xact IDs < xmin are considered finished.
 *        All xact IDs >= xmax are considered still running.
 *        For an xact ID xmin <= xid < xmax, consult list to see whether
 *        it is considered running or not.
 * This ensures that the set of transactions seen as "running" by the
 * current xact will not change after it takes the snapshot.
 *
 * All running top-level XIDs are included in the snapshot, except for lazy
 * VACUUM processes.  We also try to include running subtransaction XIDs,
 * but since PGPROC has only a limited cache area for subxact XIDs, full
 * information may not be available.  If we find any overflowed subxid arrays,
 * we have to mark the snapshot's subxid data as overflowed, and extra work
 * *may* need to be done to determine what's running (see XidInMVCCSnapshot()
 * in tqual.c).
 *
 * We also update the following backend-global variables:
 *        TransactionXmin: the oldest xmin of any snapshot in use in the
 *            current transaction (this is the same as MyPgXact->xmin).
 *        RecentXmin: the xmin computed for the most recent snapshot.  XIDs
 *            older than this are known not running any more.
 *        RecentGlobalXmin: the global xmin (oldest TransactionXmin across all
 *            running transactions, except those running LAZY VACUUM).  This is
 *            the same computation done by
 *            GetOldestXmin(NULL, PROCARRAY_FLAGS_VACUUM).
 *        RecentGlobalDataXmin: the global xmin for non-catalog tables
 *            >= RecentGlobalXmin
 *
 * Note: this function should probably not be called with an argument that's
 * not statically allocated (see xip allocation below).
 */
Snapshot
GetSnapshotData_shard(Snapshot snapshot, bool latest, bool need_shardmap)
{// #lizard forgives
    ProcArrayStruct *arrayP = procArray;
    TransactionId xmin;
    TransactionId xmax;
    TransactionId globalxmin;
#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    TransactionId prepare_xmin;
    GlobalTimestamp tmin;
    int            precount = 0;
    int            subprecount = 0;
#endif
    int            index;
    int            count = 0;
    int            subcount = 0;
    bool        suboverflowed = false;
    volatile TransactionId replication_slot_xmin = InvalidTransactionId;
    volatile TransactionId replication_slot_catalog_xmin = InvalidTransactionId;
#ifdef __USE_GLOBAL_SNAPSHOT__
    TransactionId clustermon_xmin PG_USED_FOR_ASSERTS_ONLY;
#endif

#ifdef _MIGRATE_
    if(IS_PGXC_DATANODE && IsNormalProcessingMode() && need_shardmap)
    {
        snapshot->groupsize = GetGroupSize();
        if(!SnapshotGetShardTable(snapshot))
        {
            SnapshotGetShardTable(snapshot) = (Bitmapset *)snapshot->sg_filler;
        }
        (void)CopyShardGroups_DN(SnapshotGetShardTable(snapshot));
    }
    else
    {
        snapshot->groupsize= 0;
        SnapshotGetShardTable(snapshot) = NULL;
    }
#endif

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__ /* PGXC_DATANODE */
    snapshot->local = false;
    /*
     * If the user has chosen to work with a coordinator-local snapshot, just
     * compute snapshot locally. This can have adverse effects on the global
     * consistency, in a multi-coordinator environment, but also in a
     * single-coordinator setup because our recent changes to transaction
     * management now allows datanodes to start global snapshots or more
     * precisely attach current transaction to a global transaction. But users
     * may still want to use this model for performance of their XL cluster, at
     * the cost of reduced global consistency
     */ 

    if (GlobalSnapshotSource == GLOBAL_SNAPSHOT_SOURCE_GTM)
    {
        /*
         * Obtain a global snapshot for a Postgres-XC session
         * if possible.
         */
        (void) GetPGXCSnapshotData(snapshot, latest);
    }
    
#endif

    /*
     * Fallback to standard routine, calculate snapshot from local proc arrey
     * if no master connection
     */
    Assert(snapshot != NULL);

    /*
     * Allocating space for maxProcs xids is usually overkill; numProcs would
     * be sufficient.  But it seems better to do the malloc while not holding
     * the lock, so we can't look at numProcs.  Likewise, we allocate much
     * more subxip storage than is probably needed.
     *
     * This does open a possibility for avoiding repeated malloc/free: since
     * maxProcs does not change at runtime, we can simply reuse the previous
     * xip arrays if any.  (This relies on the fact that all callers pass
     * static SnapshotData structs.)
     */
    if (snapshot->xip == NULL)
    {
        /*
         * First call for this snapshot. Snapshot is same size whether or not
         * we are in recovery, see later comments.
         */
        snapshot->xip = (TransactionId *)
            malloc(GetMaxSnapshotXidCount() * sizeof(TransactionId));
        if (snapshot->xip == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
        Assert(snapshot->subxip == NULL);
        snapshot->subxip = (TransactionId *)
            malloc(GetMaxSnapshotSubxidCount() * sizeof(TransactionId));
        if (snapshot->subxip == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
        snapshot->prepare_xip = (TransactionId *)
            malloc(GetMaxSnapshotXidCount() * sizeof(TransactionId));
        if (snapshot->prepare_xip == NULL)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
        }

        snapshot->prepare_subxip = (TransactionId *)
            malloc(GetMaxSnapshotSubxidCount() * sizeof(TransactionId));
        if (snapshot->prepare_subxip == NULL)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
        }

        snapshot->prepare_xip_ts = (GlobalTimestamp *)
            malloc(GetMaxSnapshotXidCount() * sizeof(GlobalTimestamp));
        if (snapshot->prepare_xip_ts == NULL)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
        }

        
        snapshot->prepare_subxip_ts = (GlobalTimestamp *)
            malloc(GetMaxSnapshotSubxidCount() * sizeof(GlobalTimestamp));
        if (snapshot->prepare_subxip_ts == NULL)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
        }
#endif

    }

    /*
     * It is sufficient to get shared lock on ProcArrayLock, even if we are
     * going to set MyPgXact->xmin.
     */
    LWLockAcquire(ProcArrayLock, LW_SHARED);

    /* xmax is always latestCompletedXid + 1 */
    xmax = ShmemVariableCache->latestCompletedXid;
    Assert(TransactionIdIsNormal(xmax));
    TransactionIdAdvance(xmax);

    /* initialize xmin calculation with xmax */
    globalxmin = xmin = xmax;
#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    prepare_xmin   = xmin;
    RecentCommitTs = tmin = ShmemVariableCache->latestGTS > ShmemVariableCache->latestCommitTs ? 
                                ShmemVariableCache->latestGTS : ShmemVariableCache->latestCommitTs;
#endif

    snapshot->takenDuringRecovery = RecoveryInProgress();

    if (!snapshot->takenDuringRecovery)
    {
        int           *pgprocnos = arrayP->pgprocnos;
        int            numProcs;

        /*
         * Spin over procArray checking xid, xmin, and subxids.  The goal is
         * to gather all active xids, find the lowest xmin, and try to record
         * subxids.
         */
        numProcs = arrayP->numProcs;
        for (index = 0; index < numProcs; index++)
        {
            int            pgprocno = pgprocnos[index];
            volatile PGXACT *pgxact = &allPgXact[pgprocno];
            TransactionId xid;
            GlobalTimestamp saved_tmin;


#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__

            saved_tmin = pg_atomic_read_u64(&pgxact->tmin);
            if(GlobalTimestampIsValid(saved_tmin) && (saved_tmin < tmin))
            {
                tmin = saved_tmin;
            }
            /* Fetch xid just once - see GetNewTransactionId */
            xid = pgxact->xid;

            if(TransactionIdIsNormal(xid))
            {                    
                if(NormalTransactionIdPrecedes(xid, prepare_xmin))
                {
                    prepare_xmin = xid;
                }

                if(pgxact != MyPgXact)
                {

                    GlobalTimestamp prepare_ts = pg_atomic_read_u64(&pgxact->prepare_timestamp);
                    TransactionId latest_xid = pgxact->xid;
                    /*
                     * Fetching the xid and checking whether it has been changed 
                     * to ensure that the prepare_timestamp indeed belongs to this xid and is valid.
                     */
                    if(latest_xid != xid)
                    {
                        goto prepare_finished;
                    }
                    
                    snapshot->prepare_xip[precount] = xid;
                    snapshot->prepare_xip_ts[precount++] = prepare_ts;
                    
                    if (!suboverflowed)
                    {
                        if (pgxact->overflowed)
                        {
                            suboverflowed = true;
                        }
                        else
                        {
                            int         nxids = pgxact->nxids;
                            int         j;

                            if(nxids > 0)
                            {
                                volatile PGPROC *proc = &allProcs[pgprocno];

                                for(j = 0; j < nxids; j++)
                                {
                                    /* Fetch xid just once - see GetNewTransactionId */
                                    TransactionId cxid = proc->subxids.xids[j];

                                    snapshot->prepare_subxip[subprecount] = cxid;
                                    snapshot->prepare_subxip_ts[subprecount++] = prepare_ts;
                                }
                            }
                        }
                    }
                }
                
            }
prepare_finished:
#endif        


            /*
             * Backend is doing logical decoding which manages xmin
             * separately, check below.
             */
            if (pgxact->vacuumFlags & PROC_IN_LOGICAL_DECODING)
                continue;

            /* Ignore procs running LAZY VACUUM */
            if (pgxact->vacuumFlags & PROC_IN_VACUUM)
                continue;

            /* Update globalxmin to be the smallest valid xmin */
            xid = pgxact->xmin; /* fetch just once */
            if (TransactionIdIsNormal(xid) &&
                NormalTransactionIdPrecedes(xid, globalxmin))
                globalxmin = xid;

            /* Fetch xid just once - see GetNewTransactionId */
            xid = pgxact->xid;

            /*
             * If the transaction has no XID assigned, we can skip it; it
             * won't have sub-XIDs either.  If the XID is >= xmax, we can also
             * skip it; such transactions will be treated as running anyway
             * (and any sub-XIDs will also be >= xmax).
             */
            if (!TransactionIdIsNormal(xid)
                || !NormalTransactionIdPrecedes(xid, xmax))
                continue;

            /*
             * We don't include our own XIDs (if any) in the snapshot, but we
             * must include them in xmin.
             */
            if (NormalTransactionIdPrecedes(xid, xmin))
                xmin = xid;
            if (pgxact == MyPgXact)
                continue;

            /* Add XID to snapshot. */
            snapshot->xip[count++] = xid;

            /*
             * Save subtransaction XIDs if possible (if we've already
             * overflowed, there's no point).  Note that the subxact XIDs must
             * be later than their parent, so no need to check them against
             * xmin.  We could filter against xmax, but it seems better not to
             * do that much work while holding the ProcArrayLock.
             *
             * The other backend can add more subxids concurrently, but cannot
             * remove any.  Hence it's important to fetch nxids just once.
             * Should be safe to use memcpy, though.  (We needn't worry about
             * missing any xids added concurrently, because they must postdate
             * xmax.)
             *
             * Again, our own XIDs are not included in the snapshot.
             */
            if (!suboverflowed)
            {
                if (pgxact->overflowed)
                    suboverflowed = true;
                else
                {
                    int            nxids = pgxact->nxids;

                    if (nxids > 0)
                    {
                        volatile PGPROC *proc = &allProcs[pgprocno];

                        memcpy(snapshot->subxip + subcount,
                               (void *) proc->subxids.xids,
                               nxids * sizeof(TransactionId));
                        subcount += nxids;
                    }
                }
            }
        }
    }
    else
    {
        /*
         * We're in hot standby, so get XIDs from KnownAssignedXids.
         *
         * We store all xids directly into subxip[]. Here's why:
         *
         * In recovery we don't know which xids are top-level and which are
         * subxacts, a design choice that greatly simplifies xid processing.
         *
         * It seems like we would want to try to put xids into xip[] only, but
         * that is fairly small. We would either need to make that bigger or
         * to increase the rate at which we WAL-log xid assignment; neither is
         * an appealing choice.
         *
         * We could try to store xids into xip[] first and then into subxip[]
         * if there are too many xids. That only works if the snapshot doesn't
         * overflow because we do not search subxip[] in that case. A simpler
         * way is to just store all xids in the subxact array because this is
         * by far the bigger array. We just leave the xip array empty.
         *
         * Either way we need to change the way XidInMVCCSnapshot() works
         * depending upon when the snapshot was taken, or change normal
         * snapshot processing so it matches.
         *
         * Note: It is possible for recovery to end before we finish taking
         * the snapshot, and for newly assigned transaction ids to be added to
         * the ProcArray.  xmax cannot change while we hold ProcArrayLock, so
         * those newly added transaction ids would be filtered away, so we
         * need not be concerned about them.
         */
        subcount = KnownAssignedXidsGetAndSetXmin(snapshot->subxip, &xmin,
                                                  xmax);

        if (TransactionIdPrecedesOrEquals(xmin, procArray->lastOverflowedXid))
            suboverflowed = true;
    }


    /* fetch into volatile var while ProcArrayLock is held */
    replication_slot_xmin = procArray->replication_slot_xmin;
    replication_slot_catalog_xmin = procArray->replication_slot_catalog_xmin;

    if (!TransactionIdIsValid(MyPgXact->xmin))
        MyPgXact->xmin = TransactionXmin = xmin;
#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__

    if(!snapshot->local)
    {
        pg_atomic_write_u64(&MyPgXact->tmin,snapshot->start_ts);
    }
    
    if(enable_distri_print)
    {
        elog(LOG, "MyPgXact xid %d tmin "INT64_FORMAT " start ts " INT64_FORMAT, 
                        MyPgXact->xid, pg_atomic_read_u64(&MyPgXact->tmin), snapshot->start_ts);
    }
#endif


    LWLockRelease(ProcArrayLock);

    /*
     * Update globalxmin to include actual process xids.  This is a slightly
     * different way of computing it than GetOldestXmin uses, but should give
     * the same result.
     */
    if (TransactionIdPrecedes(xmin, globalxmin))
        globalxmin = xmin;

#ifdef __USE_GLOBAL_SNAPSHOT__
    clustermon_xmin = ClusterMonitorGetGlobalXmin();
    if (TransactionIdPrecedes(clustermon_xmin, globalxmin))
        globalxmin = clustermon_xmin;
#endif

    /* Update global variables too */
    RecentGlobalXmin = globalxmin - vacuum_defer_cleanup_age;
    if (!TransactionIdIsNormal(RecentGlobalXmin))
        RecentGlobalXmin = FirstNormalTransactionId;

    /* Check whether there's a replication slot requiring an older xmin. */
    if (TransactionIdIsValid(replication_slot_xmin) &&
        NormalTransactionIdPrecedes(replication_slot_xmin, RecentGlobalXmin))
        RecentGlobalXmin = replication_slot_xmin;

    /* Non-catalog tables can be vacuumed if older than this xid */
    RecentGlobalDataXmin = RecentGlobalXmin;

    /*
     * Check whether there's a replication slot requiring an older catalog
     * xmin.
     */
    if (TransactionIdIsNormal(replication_slot_catalog_xmin) &&
        NormalTransactionIdPrecedes(replication_slot_catalog_xmin, RecentGlobalXmin))
        RecentGlobalXmin = replication_slot_catalog_xmin;

    RecentXmin = xmin;

    snapshot->xmin = xmin;
    snapshot->xmax = xmax;
    snapshot->xcnt = count;
    snapshot->subxcnt = subcount;
    snapshot->suboverflowed = suboverflowed;

    snapshot->curcid = GetCurrentCommandId(false);

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__
    snapshot->prepare_xcnt = precount;
    snapshot->prepare_subxcnt = subprecount;
    if(TransactionIdPrecedes(xmin, prepare_xmin))
    {
        prepare_xmin = xmin;
    }
    snapshot->prepare_xmin = prepare_xmin;


    {
        if(RecentCommitTs < (vacuum_delta * TIMESTAMP_SHIFT))
        {
            RecentCommitTs = InvalidGlobalTimestamp;
        }
        else
        {
            RecentCommitTs = RecentCommitTs - (vacuum_delta * TIMESTAMP_SHIFT);
        }
        
        if(!snapshot->local 
            && TestForOldTimestamp(snapshot->start_ts, RecentCommitTs))
        {
            ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("start timestamp " INT64_FORMAT " is too old to execute, recentCommitTs " INT64_FORMAT , 
                    snapshot->start_ts, RecentCommitTs + (vacuum_delta * TIMESTAMP_SHIFT))));
        }
    }
    
    if(!snapshot->local && (snapshot->start_ts < tmin))
    {
        tmin = snapshot->start_ts;
    }

    if(tmin <  (vacuum_delta * TIMESTAMP_SHIFT))
    {
        RecentDataTs = InvalidGlobalTimestamp;
    }
    else
    {
        RecentDataTs = tmin -  (vacuum_delta * TIMESTAMP_SHIFT);
    }
    
#endif

#ifdef __USE_GLOBAL_SNAPSHOT__
    if (!RecoveryInProgress())
        elog(DEBUG1, "Local snapshot is built, xmin: %d, xmax: %d, xcnt: %d, RecentGlobalXmin: %d",
             xmin, xmax, count, globalxmin);
#endif

    /*
     * This is a new snapshot, so set both refcounts are zero, and mark it as
     * not copied in persistent memory.
     */
    snapshot->active_count = 0;
    snapshot->regd_count = 0;
    snapshot->copied = false;

    if (old_snapshot_threshold < 0)
    {
        /*
         * If not using "snapshot too old" feature, fill related fields with
         * dummy values that don't require any locking.
         */
        snapshot->lsn = InvalidXLogRecPtr;
        snapshot->whenTaken = 0;
    }
    else
    {
        /*
         * Capture the current time and WAL stream location in case this
         * snapshot becomes old enough to need to fall back on the special
         * "old snapshot" logic.
         */
        snapshot->lsn = GetXLogInsertRecPtr();
        snapshot->whenTaken = GetSnapshotCurrentTimestamp();
        MaintainOldSnapshotTimeMapping(snapshot->whenTaken, xmin);
    }

    return snapshot;
}

/*
 * ProcArrayInstallImportedXmin -- install imported xmin into MyPgXact->xmin
 *
 * This is called when installing a snapshot imported from another
 * transaction.  To ensure that OldestXmin doesn't go backwards, we must
 * check that the source transaction is still running, and we'd better do
 * that atomically with installing the new xmin.
 *
 * Returns TRUE if successful, FALSE if source xact is no longer running.
 */
bool
ProcArrayInstallImportedXmin(TransactionId xmin,
                             VirtualTransactionId *sourcevxid)
{// #lizard forgives
    bool        result = false;
    ProcArrayStruct *arrayP = procArray;
    int            index;

    Assert(TransactionIdIsNormal(xmin));
    if (!sourcevxid)
        return false;

    /* Get lock so source xact can't end while we're doing this */
    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        TransactionId xid;

        /* Ignore procs running LAZY VACUUM */
        if (pgxact->vacuumFlags & PROC_IN_VACUUM)
            continue;

        /* We are only interested in the specific virtual transaction. */
        if (proc->backendId != sourcevxid->backendId)
            continue;
        if (proc->lxid != sourcevxid->localTransactionId)
            continue;

        /*
         * We check the transaction's database ID for paranoia's sake: if it's
         * in another DB then its xmin does not cover us.  Caller should have
         * detected this already, so we just treat any funny cases as
         * "transaction not found".
         */
        if (proc->databaseId != MyDatabaseId)
            continue;

        /*
         * Likewise, let's just make real sure its xmin does cover us.
         */
        xid = pgxact->xmin;        /* fetch just once */
        if (!TransactionIdIsNormal(xid) ||
            !TransactionIdPrecedesOrEquals(xid, xmin))
            continue;

        /*
         * We're good.  Install the new xmin.  As in GetSnapshotData, set
         * TransactionXmin too.  (Note that because snapmgr.c called
         * GetSnapshotData first, we'll be overwriting a valid xmin here, so
         * we don't check that.)
         */
        MyPgXact->xmin = TransactionXmin = xmin;

        result = true;
        break;
    }

    LWLockRelease(ProcArrayLock);

    return result;
}

/*
 * ProcArrayInstallRestoredXmin -- install restored xmin into MyPgXact->xmin
 *
 * This is like ProcArrayInstallImportedXmin, but we have a pointer to the
 * PGPROC of the transaction from which we imported the snapshot, rather than
 * an XID.
 *
 * Returns TRUE if successful, FALSE if source xact is no longer running.
 */
bool
ProcArrayInstallRestoredXmin(TransactionId xmin, PGPROC *proc)
{
    bool        result = false;
    TransactionId xid;
    volatile PGXACT *pgxact;

    Assert(TransactionIdIsNormal(xmin));
    Assert(proc != NULL);

    /* Get lock so source xact can't end while we're doing this */
    LWLockAcquire(ProcArrayLock, LW_SHARED);

    pgxact = &allPgXact[proc->pgprocno];

    /*
     * Be certain that the referenced PGPROC has an advertised xmin which is
     * no later than the one we're installing, so that the system-wide xmin
     * can't go backwards.  Also, make sure it's running in the same database,
     * so that the per-database xmin cannot go backwards.
     */
    xid = pgxact->xmin;            /* fetch just once */
    if (proc->databaseId == MyDatabaseId &&
        TransactionIdIsNormal(xid) &&
        TransactionIdPrecedesOrEquals(xid, xmin))
    {
        MyPgXact->xmin = TransactionXmin = xmin;
        result = true;
    }

    LWLockRelease(ProcArrayLock);

    return result;
}

/*
 * GetRunningTransactionData -- returns information about running transactions.
 *
 * Similar to GetSnapshotData but returns more information. We include
 * all PGXACTs with an assigned TransactionId, even VACUUM processes and
 * prepared transactions.
 *
 * We acquire XidGenLock and ProcArrayLock, but the caller is responsible for
 * releasing them. Acquiring XidGenLock ensures that no new XIDs enter the proc
 * array until the caller has WAL-logged this snapshot, and releases the
 * lock. Acquiring ProcArrayLock ensures that no transactions commit until the
 * lock is released.
 *
 * The returned data structure is statically allocated; caller should not
 * modify it, and must not assume it is valid past the next call.
 *
 * This is never executed during recovery so there is no need to look at
 * KnownAssignedXids.
 *
 * Dummy PGXACTs from prepared transaction are included, meaning that this
 * may return entries with duplicated TransactionId values coming from
 * transaction finishing to prepare.  Nothing is done about duplicated
 * entries here to not hold on ProcArrayLock more than necessary.
 *
 * We don't worry about updating other counters, we want to keep this as
 * simple as possible and leave GetSnapshotData() as the primary code for
 * that bookkeeping.
 *
 * Note that if any transaction has overflowed its cached subtransactions
 * then there is no real need include any subtransactions. That isn't a
 * common enough case to worry about optimising the size of the WAL record,
 * and we may wish to see that data for diagnostic purposes anyway.
 */
RunningTransactions
GetRunningTransactionData(void)
{// #lizard forgives
    /* result workspace */
    static RunningTransactionsData CurrentRunningXactsData;

    ProcArrayStruct *arrayP = procArray;
    RunningTransactions CurrentRunningXacts = &CurrentRunningXactsData;
    TransactionId latestCompletedXid;
    TransactionId oldestRunningXid;
    TransactionId *xids;
    int            index;
    int            count;
    int            subcount;
    bool        suboverflowed;

    Assert(!RecoveryInProgress());

    /*
     * Allocating space for maxProcs xids is usually overkill; numProcs would
     * be sufficient.  But it seems better to do the malloc while not holding
     * the lock, so we can't look at numProcs.  Likewise, we allocate much
     * more subxip storage than is probably needed.
     *
     * Should only be allocated in bgwriter, since only ever executed during
     * checkpoints.
     */
    if (CurrentRunningXacts->xids == NULL)
    {
        /*
         * First call
         */
        CurrentRunningXacts->xids = (TransactionId *)
            malloc(TOTAL_MAX_CACHED_SUBXIDS * sizeof(TransactionId));
        if (CurrentRunningXacts->xids == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
    }

    xids = CurrentRunningXacts->xids;

    count = subcount = 0;
    suboverflowed = false;

    /*
     * Ensure that no xids enter or leave the procarray while we obtain
     * snapshot.
     */
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    LWLockAcquire(XidGenLock, LW_SHARED);

    latestCompletedXid = ShmemVariableCache->latestCompletedXid;

    oldestRunningXid = ShmemVariableCache->nextXid;

    /*
     * Spin over procArray collecting all xids
     */
    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        TransactionId xid;

        /* Fetch xid just once - see GetNewTransactionId */
        xid = pgxact->xid;

        /*
         * We don't need to store transactions that don't have a TransactionId
         * yet because they will not show as running on a standby server.
         */
        if (!TransactionIdIsValid(xid))
            continue;

        xids[count++] = xid;

        if (TransactionIdPrecedes(xid, oldestRunningXid))
            oldestRunningXid = xid;

        if (pgxact->overflowed)
            suboverflowed = true;
    }

    /*
     * Spin over procArray collecting all subxids, but only if there hasn't
     * been a suboverflow.
     */
    if (!suboverflowed)
    {
        for (index = 0; index < arrayP->numProcs; index++)
        {
            int            pgprocno = arrayP->pgprocnos[index];
            volatile PGPROC *proc = &allProcs[pgprocno];
            volatile PGXACT *pgxact = &allPgXact[pgprocno];
            int            nxids;

            /*
             * Save subtransaction XIDs. Other backends can't add or remove
             * entries while we're holding XidGenLock.
             */
            nxids = pgxact->nxids;
            if (nxids > 0)
            {
                memcpy(&xids[count], (void *) proc->subxids.xids,
                       nxids * sizeof(TransactionId));
                count += nxids;
                subcount += nxids;

                /*
                 * Top-level XID of a transaction is always less than any of
                 * its subxids, so we don't need to check if any of the
                 * subxids are smaller than oldestRunningXid
                 */
            }
        }
    }

    /*
     * It's important *not* to include the limits set by slots here because
     * snapbuild.c uses oldestRunningXid to manage its xmin horizon. If those
     * were to be included here the initial value could never increase because
     * of a circular dependency where slots only increase their limits when
     * running xacts increases oldestRunningXid and running xacts only
     * increases if slots do.
     */

    CurrentRunningXacts->xcnt = count - subcount;
    CurrentRunningXacts->subxcnt = subcount;
    CurrentRunningXacts->subxid_overflow = suboverflowed;
    CurrentRunningXacts->nextXid = ShmemVariableCache->nextXid;
    CurrentRunningXacts->oldestRunningXid = oldestRunningXid;
    CurrentRunningXacts->latestCompletedXid = latestCompletedXid;

    Assert(TransactionIdIsValid(CurrentRunningXacts->nextXid));
    Assert(TransactionIdIsValid(CurrentRunningXacts->oldestRunningXid));
    Assert(TransactionIdIsNormal(CurrentRunningXacts->latestCompletedXid));

    /* We don't release the locks here, the caller is responsible for that */

    return CurrentRunningXacts;
}

/*
 * GetOldestActiveTransactionId()
 *
 * Similar to GetSnapshotData but returns just oldestActiveXid. We include
 * all PGXACTs with an assigned TransactionId, even VACUUM processes.
 * We look at all databases, though there is no need to include WALSender
 * since this has no effect on hot standby conflicts.
 *
 * This is never executed during recovery so there is no need to look at
 * KnownAssignedXids.
 *
 * We don't worry about updating other counters, we want to keep this as
 * simple as possible and leave GetSnapshotData() as the primary code for
 * that bookkeeping.
 */
TransactionId
GetOldestActiveTransactionId(void)
{
    ProcArrayStruct *arrayP = procArray;
    TransactionId oldestRunningXid;
    int            index;

    Assert(!RecoveryInProgress());

    /*
     * Read nextXid, as the upper bound of what's still active.
     *
     * Reading a TransactionId is atomic, but we must grab the lock to make
     * sure that all XIDs < nextXid are already present in the proc array (or
     * have already completed), when we spin over it.
     */
    LWLockAcquire(XidGenLock, LW_SHARED);
    oldestRunningXid = ShmemVariableCache->nextXid;
    LWLockRelease(XidGenLock);

    /*
     * Spin over procArray collecting all xids and subxids.
     */
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        TransactionId xid;

        /* Fetch xid just once - see GetNewTransactionId */
        xid = pgxact->xid;

        if (!TransactionIdIsNormal(xid))
            continue;

        if (TransactionIdPrecedes(xid, oldestRunningXid))
            oldestRunningXid = xid;

        /*
         * Top-level XID of a transaction is always less than any of its
         * subxids, so we don't need to check if any of the subxids are
         * smaller than oldestRunningXid
         */
    }
    LWLockRelease(ProcArrayLock);

    return oldestRunningXid;
}

/*
 * GetOldestSafeDecodingTransactionId -- lowest xid not affected by vacuum
 *
 * Returns the oldest xid that we can guarantee not to have been affected by
 * vacuum, i.e. no rows >= that xid have been vacuumed away unless the
 * transaction aborted. Note that the value can (and most of the time will) be
 * much more conservative than what really has been affected by vacuum, but we
 * currently don't have better data available.
 *
 * This is useful to initialize the cutoff xid after which a new changeset
 * extraction replication slot can start decoding changes.
 *
 * Must be called with ProcArrayLock held either shared or exclusively,
 * although most callers will want to use exclusive mode since it is expected
 * that the caller will immediately use the xid to peg the xmin horizon.
 */
TransactionId
GetOldestSafeDecodingTransactionId(bool catalogOnly)
{// #lizard forgives
    ProcArrayStruct *arrayP = procArray;
    TransactionId oldestSafeXid;
    int            index;
    bool        recovery_in_progress = RecoveryInProgress();

    Assert(LWLockHeldByMe(ProcArrayLock));

    /*
     * Acquire XidGenLock, so no transactions can acquire an xid while we're
     * running. If no transaction with xid were running concurrently a new xid
     * could influence the RecentXmin et al.
     *
     * We initialize the computation to nextXid since that's guaranteed to be
     * a safe, albeit pessimal, value.
     */
    LWLockAcquire(XidGenLock, LW_SHARED);
    oldestSafeXid = ShmemVariableCache->nextXid;

    /*
     * If there's already a slot pegging the xmin horizon, we can start with
     * that value, it's guaranteed to be safe since it's computed by this
     * routine initially and has been enforced since.  We can always use the
     * slot's general xmin horizon, but the catalog horizon is only usable
     * when we only catalog data is going to be looked at.
     */
    if (TransactionIdIsValid(procArray->replication_slot_xmin) &&
        TransactionIdPrecedes(procArray->replication_slot_xmin,
                              oldestSafeXid))
        oldestSafeXid = procArray->replication_slot_xmin;

    if (catalogOnly &&
        TransactionIdIsValid(procArray->replication_slot_catalog_xmin) &&
        TransactionIdPrecedes(procArray->replication_slot_catalog_xmin,
                              oldestSafeXid))
        oldestSafeXid = procArray->replication_slot_catalog_xmin;

    /*
     * If we're not in recovery, we walk over the procarray and collect the
     * lowest xid. Since we're called with ProcArrayLock held and have
     * acquired XidGenLock, no entries can vanish concurrently, since
     * PGXACT->xid is only set with XidGenLock held and only cleared with
     * ProcArrayLock held.
     *
     * In recovery we can't lower the safe value besides what we've computed
     * above, so we'll have to wait a bit longer there. We unfortunately can
     * *not* use KnownAssignedXidsGetOldestXmin() since the KnownAssignedXids
     * machinery can miss values and return an older value than is safe.
     */
    if (!recovery_in_progress)
    {
        /*
         * Spin over procArray collecting all min(PGXACT->xid)
         */
        for (index = 0; index < arrayP->numProcs; index++)
        {
            int            pgprocno = arrayP->pgprocnos[index];
            volatile PGXACT *pgxact = &allPgXact[pgprocno];
            TransactionId xid;

            /* Fetch xid just once - see GetNewTransactionId */
            xid = pgxact->xid;

            if (!TransactionIdIsNormal(xid))
                continue;

            if (TransactionIdPrecedes(xid, oldestSafeXid))
                oldestSafeXid = xid;
        }
    }

    LWLockRelease(XidGenLock);

    return oldestSafeXid;
}

/*
 * GetVirtualXIDsDelayingChkpt -- Get the VXIDs of transactions that are
 * delaying checkpoint because they have critical actions in progress.
 *
 * Constructs an array of VXIDs of transactions that are currently in commit
 * critical sections, as shown by having delayChkpt set in their PGXACT.
 *
 * Returns a palloc'd array that should be freed by the caller.
 * *nvxids is the number of valid entries.
 *
 * Note that because backends set or clear delayChkpt without holding any lock,
 * the result is somewhat indeterminate, but we don't really care.  Even in
 * a multiprocessor with delayed writes to shared memory, it should be certain
 * that setting of delayChkpt will propagate to shared memory when the backend
 * takes a lock, so we cannot fail to see a virtual xact as delayChkpt if
 * it's already inserted its commit record.  Whether it takes a little while
 * for clearing of delayChkpt to propagate is unimportant for correctness.
 */
VirtualTransactionId *
GetVirtualXIDsDelayingChkpt(int *nvxids)
{
    VirtualTransactionId *vxids;
    ProcArrayStruct *arrayP = procArray;
    int            count = 0;
    int            index;

    /* allocate what's certainly enough result space */
    vxids = (VirtualTransactionId *)
        palloc(sizeof(VirtualTransactionId) * arrayP->maxProcs);

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];

        if (pgxact->delayChkpt)
        {
            VirtualTransactionId vxid;

            GET_VXID_FROM_PGPROC(vxid, *proc);
            if (VirtualTransactionIdIsValid(vxid))
                vxids[count++] = vxid;
        }
    }

    LWLockRelease(ProcArrayLock);

    *nvxids = count;
    return vxids;
}

/*
 * HaveVirtualXIDsDelayingChkpt -- Are any of the specified VXIDs delaying?
 *
 * This is used with the results of GetVirtualXIDsDelayingChkpt to see if any
 * of the specified VXIDs are still in critical sections of code.
 *
 * Note: this is O(N^2) in the number of vxacts that are/were delaying, but
 * those numbers should be small enough for it not to be a problem.
 */
bool
HaveVirtualXIDsDelayingChkpt(VirtualTransactionId *vxids, int nvxids)
{
    bool        result = false;
    ProcArrayStruct *arrayP = procArray;
    int            index;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        VirtualTransactionId vxid;

        GET_VXID_FROM_PGPROC(vxid, *proc);

        if (pgxact->delayChkpt && VirtualTransactionIdIsValid(vxid))
        {
            int            i;

            for (i = 0; i < nvxids; i++)
            {
                if (VirtualTransactionIdEquals(vxid, vxids[i]))
                {
                    result = true;
                    break;
                }
            }
            if (result)
                break;
        }
    }

    LWLockRelease(ProcArrayLock);

    return result;
}

/*
 * BackendPidGetProc -- get a backend's PGPROC given its PID
 *
 * Returns NULL if not found.  Note that it is up to the caller to be
 * sure that the question remains meaningful for long enough for the
 * answer to be used ...
 */
PGPROC *
BackendPidGetProc(int pid)
{
    PGPROC       *result;

    if (pid == 0)                /* never match dummy PGPROCs */
        return NULL;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    result = BackendPidGetProcWithLock(pid);

    LWLockRelease(ProcArrayLock);

    return result;
}

/*
 * BackendPidGetProcWithLock -- get a backend's PGPROC given its PID
 *
 * Same as above, except caller must be holding ProcArrayLock.  The found
 * entry, if any, can be assumed to be valid as long as the lock remains held.
 */
PGPROC *
BackendPidGetProcWithLock(int pid)
{
    PGPROC       *result = NULL;
    ProcArrayStruct *arrayP = procArray;
    int            index;

    if (pid == 0)                /* never match dummy PGPROCs */
        return NULL;

    for (index = 0; index < arrayP->numProcs; index++)
    {
        PGPROC       *proc = &allProcs[arrayP->pgprocnos[index]];

        if (proc->pid == pid)
        {
            result = proc;
            break;
        }
    }

    return result;
}

/*
 * BackendXidGetPid -- get a backend's pid given its XID
 *
 * Returns 0 if not found or it's a prepared transaction.  Note that
 * it is up to the caller to be sure that the question remains
 * meaningful for long enough for the answer to be used ...
 *
 * Only main transaction Ids are considered.  This function is mainly
 * useful for determining what backend owns a lock.
 *
 * Beware that not every xact has an XID assigned.  However, as long as you
 * only call this using an XID found on disk, you're safe.
 */
int
BackendXidGetPid(TransactionId xid)
{
    int            result = 0;
    ProcArrayStruct *arrayP = procArray;
    int            index;

    if (xid == InvalidTransactionId)    /* never match invalid xid */
        return 0;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];

        if (pgxact->xid == xid)
        {
            result = proc->pid;
            break;
        }
    }

    LWLockRelease(ProcArrayLock);

    return result;
}

/*
 * IsBackendPid -- is a given pid a running backend
 *
 * This is not called by the backend, but is called by external modules.
 */
bool
IsBackendPid(int pid)
{
    return (BackendPidGetProc(pid) != NULL);
}


/*
 * GetCurrentVirtualXIDs -- returns an array of currently active VXIDs.
 *
 * The array is palloc'd. The number of valid entries is returned into *nvxids.
 *
 * The arguments allow filtering the set of VXIDs returned.  Our own process
 * is always skipped.  In addition:
 *    If limitXmin is not InvalidTransactionId, skip processes with
 *        xmin > limitXmin.
 *    If excludeXmin0 is true, skip processes with xmin = 0.
 *    If allDbs is false, skip processes attached to other databases.
 *    If excludeVacuum isn't zero, skip processes for which
 *        (vacuumFlags & excludeVacuum) is not zero.
 *
 * Note: the purpose of the limitXmin and excludeXmin0 parameters is to
 * allow skipping backends whose oldest live snapshot is no older than
 * some snapshot we have.  Since we examine the procarray with only shared
 * lock, there are race conditions: a backend could set its xmin just after
 * we look.  Indeed, on multiprocessors with weak memory ordering, the
 * other backend could have set its xmin *before* we look.  We know however
 * that such a backend must have held shared ProcArrayLock overlapping our
 * own hold of ProcArrayLock, else we would see its xmin update.  Therefore,
 * any snapshot the other backend is taking concurrently with our scan cannot
 * consider any transactions as still running that we think are committed
 * (since backends must hold ProcArrayLock exclusive to commit).
 */
VirtualTransactionId *
GetCurrentVirtualXIDs(TransactionId limitXmin, bool excludeXmin0,
                      bool allDbs, int excludeVacuum,
                      int *nvxids)
{// #lizard forgives
    VirtualTransactionId *vxids;
    ProcArrayStruct *arrayP = procArray;
    int            count = 0;
    int            index;

    /* allocate what's certainly enough result space */
    vxids = (VirtualTransactionId *)
        palloc(sizeof(VirtualTransactionId) * arrayP->maxProcs);

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];

        if (proc == MyProc)
            continue;

        if (excludeVacuum & pgxact->vacuumFlags)
            continue;

        if (allDbs || proc->databaseId == MyDatabaseId)
        {
            /* Fetch xmin just once - might change on us */
            TransactionId pxmin = pgxact->xmin;

            if (excludeXmin0 && !TransactionIdIsValid(pxmin))
                continue;

            /*
             * InvalidTransactionId precedes all other XIDs, so a proc that
             * hasn't set xmin yet will not be rejected by this test.
             */
            if (!TransactionIdIsValid(limitXmin) ||
                TransactionIdPrecedesOrEquals(pxmin, limitXmin))
            {
                VirtualTransactionId vxid;

                GET_VXID_FROM_PGPROC(vxid, *proc);
                if (VirtualTransactionIdIsValid(vxid))
                    vxids[count++] = vxid;
            }
        }
    }

    LWLockRelease(ProcArrayLock);

    *nvxids = count;
    return vxids;
}

/*
 * GetConflictingVirtualXIDs -- returns an array of currently active VXIDs.
 *
 * Usage is limited to conflict resolution during recovery on standby servers.
 * limitXmin is supplied as either latestRemovedXid, or InvalidTransactionId
 * in cases where we cannot accurately determine a value for latestRemovedXid.
 *
 * If limitXmin is InvalidTransactionId then we want to kill everybody,
 * so we're not worried if they have a snapshot or not, nor does it really
 * matter what type of lock we hold.
 *
 * All callers that are checking xmins always now supply a valid and useful
 * value for limitXmin. The limitXmin is always lower than the lowest
 * numbered KnownAssignedXid that is not already a FATAL error. This is
 * because we only care about cleanup records that are cleaning up tuple
 * versions from committed transactions. In that case they will only occur
 * at the point where the record is less than the lowest running xid. That
 * allows us to say that if any backend takes a snapshot concurrently with
 * us then the conflict assessment made here would never include the snapshot
 * that is being derived. So we take LW_SHARED on the ProcArray and allow
 * concurrent snapshots when limitXmin is valid. We might think about adding
 *     Assert(limitXmin < lowest(KnownAssignedXids))
 * but that would not be true in the case of FATAL errors lagging in array,
 * but we already know those are bogus anyway, so we skip that test.
 *
 * If dbOid is valid we skip backends attached to other databases.
 *
 * Be careful to *not* pfree the result from this function. We reuse
 * this array sufficiently often that we use malloc for the result.
 */
VirtualTransactionId *
GetConflictingVirtualXIDs(TransactionId limitXmin, Oid dbOid)
{// #lizard forgives
    static VirtualTransactionId *vxids;
    ProcArrayStruct *arrayP = procArray;
    int            count = 0;
    int            index;

    /*
     * If first time through, get workspace to remember main XIDs in. We
     * malloc it permanently to avoid repeated palloc/pfree overhead. Allow
     * result space, remembering room for a terminator.
     */
    if (vxids == NULL)
    {
        vxids = (VirtualTransactionId *)
            malloc(sizeof(VirtualTransactionId) * (arrayP->maxProcs + 1));
        if (vxids == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
    }

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];

        /* Exclude prepared transactions */
        if (proc->pid == 0)
            continue;

        if (!OidIsValid(dbOid) ||
            proc->databaseId == dbOid)
        {
            /* Fetch xmin just once - can't change on us, but good coding */
            TransactionId pxmin = pgxact->xmin;

            /*
             * We ignore an invalid pxmin because this means that backend has
             * no snapshot currently. We hold a Share lock to avoid contention
             * with users taking snapshots.  That is not a problem because the
             * current xmin is always at least one higher than the latest
             * removed xid, so any new snapshot would never conflict with the
             * test here.
             */
            if (!TransactionIdIsValid(limitXmin) ||
                (TransactionIdIsValid(pxmin) && !TransactionIdFollows(pxmin, limitXmin)))
            {
                VirtualTransactionId vxid;

                GET_VXID_FROM_PGPROC(vxid, *proc);
                if (VirtualTransactionIdIsValid(vxid))
                    vxids[count++] = vxid;
            }
        }
    }

    LWLockRelease(ProcArrayLock);

    /* add the terminator */
    vxids[count].backendId = InvalidBackendId;
    vxids[count].localTransactionId = InvalidLocalTransactionId;

    return vxids;
}

/*
 * CancelVirtualTransaction - used in recovery conflict processing
 *
 * Returns pid of the process signaled, or 0 if not found.
 */
pid_t
CancelVirtualTransaction(VirtualTransactionId vxid, ProcSignalReason sigmode)
{
    ProcArrayStruct *arrayP = procArray;
    int            index;
    pid_t        pid = 0;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        VirtualTransactionId procvxid;

        GET_VXID_FROM_PGPROC(procvxid, *proc);

        if (procvxid.backendId == vxid.backendId &&
            procvxid.localTransactionId == vxid.localTransactionId)
        {
            proc->recoveryConflictPending = true;
            pid = proc->pid;
            if (pid != 0)
            {
                /*
                 * Kill the pid if it's still here. If not, that's what we
                 * wanted so ignore any errors.
                 */
                (void) SendProcSignal(pid, sigmode, vxid.backendId);
            }
            break;
        }
    }

    LWLockRelease(ProcArrayLock);

    return pid;
}

/*
 * MinimumActiveBackends --- count backends (other than myself) that are
 *        in active transactions.  Return true if the count exceeds the
 *        minimum threshold passed.  This is used as a heuristic to decide if
 *        a pre-XLOG-flush delay is worthwhile during commit.
 *
 * Do not count backends that are blocked waiting for locks, since they are
 * not going to get to run until someone else commits.
 */
bool
MinimumActiveBackends(int min)
{// #lizard forgives
    ProcArrayStruct *arrayP = procArray;
    int            count = 0;
    int            index;

    /* Quick short-circuit if no minimum is specified */
    if (min == 0)
        return true;

    /*
     * Note: for speed, we don't acquire ProcArrayLock.  This is a little bit
     * bogus, but since we are only testing fields for zero or nonzero, it
     * should be OK.  The result is only used for heuristic purposes anyway...
     */
    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];

        /*
         * Since we're not holding a lock, need to be prepared to deal with
         * garbage, as someone could have incremented numProcs but not yet
         * filled the structure.
         *
         * If someone just decremented numProcs, 'proc' could also point to a
         * PGPROC entry that's no longer in the array. It still points to a
         * PGPROC struct, though, because freed PGPROC entries just go to the
         * free list and are recycled. Its contents are nonsense in that case,
         * but that's acceptable for this function.
         */
        if (pgprocno == -1)
            continue;            /* do not count deleted entries */
        if (proc == MyProc)
            continue;            /* do not count myself */
        if (pgxact->xid == InvalidTransactionId)
            continue;            /* do not count if no XID assigned */
        if (proc->pid == 0)
            continue;            /* do not count prepared xacts */
        if (proc->waitLock != NULL)
            continue;            /* do not count if blocked on a lock */
        count++;
        if (count >= min)
            break;
    }

    return count >= min;
}

/*
 * CountDBBackends --- count backends that are using specified database
 */
int
CountDBBackends(Oid databaseid)
{
    ProcArrayStruct *arrayP = procArray;
    int            count = 0;
    int            index;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];

        if (proc->pid == 0)
            continue;            /* do not count prepared xacts */
        if (!OidIsValid(databaseid) ||
            proc->databaseId == databaseid)
            count++;
    }

    LWLockRelease(ProcArrayLock);

    return count;
}

/*
 * CountDBConnections --- counts database backends ignoring any background
 *        worker processes
 */
int
CountDBConnections(Oid databaseid)
{
    ProcArrayStruct *arrayP = procArray;
    int            count = 0;
    int            index;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];

        if (proc->pid == 0)
            continue;            /* do not count prepared xacts */
        if (proc->isBackgroundWorker)
            continue;            /* do not count background workers */
        if (!OidIsValid(databaseid) ||
            proc->databaseId == databaseid)
            count++;
    }

    LWLockRelease(ProcArrayLock);

    return count;
}

/*
 * CancelDBBackends --- cancel backends that are using specified database
 */
void
CancelDBBackends(Oid databaseid, ProcSignalReason sigmode, bool conflictPending)
{
    ProcArrayStruct *arrayP = procArray;
    int            index;
    pid_t        pid = 0;

    /* tell all backends to die */
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];

        if (databaseid == InvalidOid || proc->databaseId == databaseid)
        {
            VirtualTransactionId procvxid;

            GET_VXID_FROM_PGPROC(procvxid, *proc);

            proc->recoveryConflictPending = conflictPending;
            pid = proc->pid;
            if (pid != 0)
            {
                /*
                 * Kill the pid if it's still here. If not, that's what we
                 * wanted so ignore any errors.
                 */
                (void) SendProcSignal(pid, sigmode, procvxid.backendId);
            }
        }
    }

    LWLockRelease(ProcArrayLock);
}

/*
 * CountUserBackends --- count backends that are used by specified user
 */
int
CountUserBackends(Oid roleid)
{
    ProcArrayStruct *arrayP = procArray;
    int            count = 0;
    int            index;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];

        if (proc->pid == 0)
            continue;            /* do not count prepared xacts */
        if (proc->isBackgroundWorker)
            continue;            /* do not count background workers */
        if (proc->roleId == roleid)
            count++;
    }

    LWLockRelease(ProcArrayLock);

    return count;
}

/*
 * CountOtherDBBackends -- check for other backends running in the given DB
 *
 * If there are other backends in the DB, we will wait a maximum of 5 seconds
 * for them to exit.  Autovacuum backends are encouraged to exit early by
 * sending them SIGTERM, but normal user backends are just waited for.
 *
 * The current backend is always ignored; it is caller's responsibility to
 * check whether the current backend uses the given DB, if it's important.
 *
 * Returns TRUE if there are (still) other backends in the DB, FALSE if not.
 * Also, *nbackends and *nprepared are set to the number of other backends
 * and prepared transactions in the DB, respectively.
 *
 * This function is used to interlock DROP DATABASE and related commands
 * against there being any active backends in the target DB --- dropping the
 * DB while active backends remain would be a Bad Thing.  Note that we cannot
 * detect here the possibility of a newly-started backend that is trying to
 * connect to the doomed database, so additional interlocking is needed during
 * backend startup.  The caller should normally hold an exclusive lock on the
 * target DB before calling this, which is one reason we mustn't wait
 * indefinitely.
 */
bool
CountOtherDBBackends(Oid databaseId, int *nbackends, int *nprepared)
{// #lizard forgives
    ProcArrayStruct *arrayP = procArray;

#define MAXAUTOVACPIDS    10        /* max autovacs to SIGTERM per iteration */
    int            autovac_pids[MAXAUTOVACPIDS];
    int            tries;

    /* 50 tries with 100ms sleep between tries makes 5 sec total wait */
    for (tries = 0; tries < 50; tries++)
    {
        int            nautovacs = 0;
        bool        found = false;
        int            index;

        CHECK_FOR_INTERRUPTS();

        *nbackends = *nprepared = 0;

        LWLockAcquire(ProcArrayLock, LW_SHARED);

        for (index = 0; index < arrayP->numProcs; index++)
        {
            int            pgprocno = arrayP->pgprocnos[index];
            volatile PGPROC *proc = &allProcs[pgprocno];
            volatile PGXACT *pgxact = &allPgXact[pgprocno];

            if (proc->databaseId != databaseId)
                continue;
            if (proc == MyProc)
                continue;
#ifdef PGXC
            /*
             * PGXC pooler just refers to XC-specific catalogs,
             * it does not create any consistency issues.
             */
            if (proc->isPooler)
                continue;
#endif

            found = true;

            if (proc->pid == 0)
                (*nprepared)++;
            else
            {
                (*nbackends)++;
                if ((pgxact->vacuumFlags & PROC_IS_AUTOVACUUM) &&
                    nautovacs < MAXAUTOVACPIDS)
                    autovac_pids[nautovacs++] = proc->pid;
            }
        }

        LWLockRelease(ProcArrayLock);

        if (!found)
            return false;        /* no conflicting backends, so done */

        /*
         * Send SIGTERM to any conflicting autovacuums before sleeping. We
         * postpone this step until after the loop because we don't want to
         * hold ProcArrayLock while issuing kill(). We have no idea what might
         * block kill() inside the kernel...
         */
        for (index = 0; index < nautovacs; index++)
            (void) kill(autovac_pids[index], SIGTERM);    /* ignore any error */

        /* sleep, then try again */
        pg_usleep(100 * 1000L); /* 100ms */
    }

    return true;                /* timed out, still conflicts */
}

#ifdef PGXC
/*
 * ReloadConnInfoOnBackends -- reload/refresh connection information
 * for all the backends
 *
 * "refresh" is less destructive than "reload"
 */
void
ReloadConnInfoOnBackends(bool refresh_only)
{
    ProcArrayStruct *arrayP = procArray;
    int            index;
    pid_t        pid = 0;

    /* tell all backends to reload except this one who already reloaded */
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        VirtualTransactionId vxid;
        GET_VXID_FROM_PGPROC(vxid, *proc);

        if (proc == MyProc)
            continue;            /* do not do that on myself */
        if (proc->isPooler)
            continue;            /* Pooler cannot do that */
        if (proc->pid == 0)
            continue;            /* useless on prepared xacts */
        if (!OidIsValid(proc->databaseId))
            continue;            /* ignore backends not connected to a database */
        if (pgxact->vacuumFlags & PROC_IN_VACUUM)
            continue;            /* ignore vacuum processes */

        pid = proc->pid;
        /*
         * Send the reload signal if backend still exists
         */
        (void) SendProcSignal(pid, refresh_only?
                      PROCSIG_PGXCPOOL_REFRESH:PROCSIG_PGXCPOOL_RELOAD,
                      vxid.backendId);
    }

    LWLockRelease(ProcArrayLock);
}
#endif

/*
 * ProcArraySetReplicationSlotXmin
 *
 * Install limits to future computations of the xmin horizon to prevent vacuum
 * and HOT pruning from removing affected rows still needed by clients with
 * replicaton slots.
 */
void
ProcArraySetReplicationSlotXmin(TransactionId xmin, TransactionId catalog_xmin,
                                bool already_locked)
{
    Assert(!already_locked || LWLockHeldByMe(ProcArrayLock));

    if (!already_locked)
        LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    procArray->replication_slot_xmin = xmin;
    procArray->replication_slot_catalog_xmin = catalog_xmin;

    if (!already_locked)
        LWLockRelease(ProcArrayLock);
}

/*
 * ProcArrayGetReplicationSlotXmin
 *
 * Return the current slot xmin limits. That's useful to be able to remove
 * data that's older than those limits.
 */
void
ProcArrayGetReplicationSlotXmin(TransactionId *xmin,
                                TransactionId *catalog_xmin)
{
    LWLockAcquire(ProcArrayLock, LW_SHARED);

    if (xmin != NULL)
        *xmin = procArray->replication_slot_xmin;

    if (catalog_xmin != NULL)
        *catalog_xmin = procArray->replication_slot_catalog_xmin;

    LWLockRelease(ProcArrayLock);
}


#define XidCacheRemove(i) \
    do { \
        MyProc->subxids.xids[i] = MyProc->subxids.xids[MyPgXact->nxids - 1]; \
        MyPgXact->nxids--; \
    } while (0)

/*
 * XidCacheRemoveRunningXids
 *
 * Remove a bunch of TransactionIds from the list of known-running
 * subtransactions for my backend.  Both the specified xid and those in
 * the xids[] array (of length nxids) are removed from the subxids cache.
 * latestXid must be the latest XID among the group.
 */
void
XidCacheRemoveRunningXids(TransactionId xid,
                          int nxids, const TransactionId *xids,
                          TransactionId latestXid)
{// #lizard forgives
    int            i,
                j;

    Assert(TransactionIdIsValid(xid));

    /*
     * We must hold ProcArrayLock exclusively in order to remove transactions
     * from the PGPROC array.  (See src/backend/access/transam/README.)  It's
     * possible this could be relaxed since we know this routine is only used
     * to abort subtransactions, but pending closer analysis we'd best be
     * conservative.
     */
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    /*
     * Under normal circumstances xid and xids[] will be in increasing order,
     * as will be the entries in subxids.  Scan backwards to avoid O(N^2)
     * behavior when removing a lot of xids.
     */
    for (i = nxids - 1; i >= 0; i--)
    {
        TransactionId anxid = xids[i];

        for (j = MyPgXact->nxids - 1; j >= 0; j--)
        {
            if (TransactionIdEquals(MyProc->subxids.xids[j], anxid))
            {
                XidCacheRemove(j);
                break;
            }
        }

        /*
         * Ordinarily we should have found it, unless the cache has
         * overflowed. However it's also possible for this routine to be
         * invoked multiple times for the same subtransaction, in case of an
         * error during AbortSubTransaction.  So instead of Assert, emit a
         * debug warning.
         */
        if (j < 0 && !MyPgXact->overflowed)
            elog(WARNING, "did not find subXID %u in MyProc", anxid);
    }

    for (j = MyPgXact->nxids - 1; j >= 0; j--)
    {
        if (TransactionIdEquals(MyProc->subxids.xids[j], xid))
        {
            XidCacheRemove(j);
            break;
        }
    }
    /* Ordinarily we should have found it, unless the cache has overflowed */
    if (j < 0 && !MyPgXact->overflowed)
        elog(WARNING, "did not find subXID %u in MyProc", xid);

    /* Also advance global latestCompletedXid while holding the lock */
    if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid,
                              latestXid))
        ShmemVariableCache->latestCompletedXid = latestXid;

    LWLockRelease(ProcArrayLock);
}

#ifdef XIDCACHE_DEBUG

/*
 * Print stats about effectiveness of XID cache
 */
static void
DisplayXidCache(void)
{
    fprintf(stderr,
            "XidCache: xmin: %ld, known: %ld, myxact: %ld, latest: %ld, mainxid: %ld, childxid: %ld, knownassigned: %ld, nooflo: %ld, slow: %ld\n",
            xc_by_recent_xmin,
            xc_by_known_xact,
            xc_by_my_xact,
            xc_by_latest_xid,
            xc_by_main_xid,
            xc_by_child_xid,
            xc_by_known_assigned,
            xc_no_overflow,
            xc_slow_answer);
}
#endif                            /* XIDCACHE_DEBUG */

void
SetGlobalTimestamp(GlobalTimestamp gts, SnapshotSource source)
{
    globalSnapshot.snapshot_source = source;
    globalSnapshot.gts = gts;
    if(enable_distri_print)
    {
        elog (LOG, "set start ts " INT64_FORMAT " xid %d", gts, GetTopTransactionIdIfAny());
    }
}


#ifdef PGXC

#if 0
/*
 * Store snapshot data received from the Coordinator
 */
void
SetGlobalSnapshotData(TransactionId xmin, TransactionId xmax,
        int xcnt, TransactionId *xip, SnapshotSource source)
{
    if (globalSnapshot.max_gxcnt < xcnt)
    {
        globalSnapshot.gxip = (TransactionId *) realloc(globalSnapshot.gxip,
                sizeof (TransactionId) * xcnt);
        if (globalSnapshot.gxip == NULL)
            elog(ERROR, "Out of memory");
        globalSnapshot.max_gxcnt = xcnt;
    }

    globalSnapshot.snapshot_source = source;
    globalSnapshot.gxmin = xmin;
    globalSnapshot.gxmax = xmax;
    globalSnapshot.gxcnt = xcnt;
    memcpy(globalSnapshot.gxip, xip, sizeof (TransactionId) * xcnt);
    elog (DEBUG1, "global snapshot info: gxmin: %d, gxmax: %d, gxcnt: %d", xmin, xmax, xcnt);
}
#endif
/*
 * Force Datanode to use local snapshot data
 */
void
UnsetGlobalSnapshotData(void)
{
    globalSnapshot.snapshot_source = SNAPSHOT_UNDEFINED;
    globalSnapshot.gxmin = InvalidTransactionId;
    globalSnapshot.gxmax = InvalidTransactionId;
    globalSnapshot.gxcnt = 0;
    globalSnapshot.gts = InvalidGlobalTimestamp;
    if (enable_distri_print)
    {
        elog (LOG, "unset snapshot info");
    }
}

#ifdef __SUPPORT_DISTRIBUTED_TRANSACTION__

/*
 * Entry of snapshot obtention for Postgres-XC node
 */
static void
GetPGXCSnapshotData(Snapshot snapshot, bool latest)
{// #lizard forgives
    /*
     * If this node is in recovery phase,
     * snapshot has to be taken directly from WAL information.
     */
    if (RecoveryInProgress() || IsInitProcessingMode() || latest)
    {
        if (IsStandbyPostgres() && !latest && query_delay)
        {
            /* need global snapshot now */
        }
        else
        {
            snapshot->local = true;
            if(enable_distri_print)
            {
                elog(LOG, "local snapshot latest %d IsinitprocessingMode %d xid %u.", latest, 
                                        IsInitProcessingMode(),
                                        GetTopTransactionIdIfAny());
            }
            return;
        }
    }
    /*
     * The typical case is that the local Coordinator passes down the snapshot to the
     * remote nodes to use, while it itself obtains it from GTM. Autovacuum processes
     * need however to connect directly to GTM themselves to obtain XID and snapshot
     * information for autovacuum worker threads.
     * A vacuum analyze uses a special function to get a transaction ID and signal
     * GTM not to include this transaction ID in snapshot.
     * A vacuum worker starts as a normal transaction would.
     */

    if(!IsPostmasterEnvironment)
    {
        snapshot->local = true;
        if(enable_distri_print)
        {
            elog(LOG, "local snapshot postmaster %d %d.", IsPostmasterEnvironment,GetTopTransactionIdIfAny()); 
        }
        return;

    }
    else if (IsConnFromCoord() || IsConnFromDatanode())
    {

        if (SNAPSHOT_COORDINATOR == globalSnapshot.snapshot_source)
        {
            
            elog(DEBUG8, "obtain timestamp from coord for snapshot postmaster %d.", IsPostmasterEnvironment); 
            GetGlobalTimestampFromGlobalSnapshot(snapshot);
        }
        else
        {
            GetGlobalTimestampFromGTM(snapshot);
        }
        return;
    }
    else 
    {
        if(enable_distri_print)
        {
            elog(LOG, "postmaster gets timestamp from GTM for snapshot latest %d GetForceXidFromGTM() %d IsInitProcessingMode %d"
                    " from coord %d.",
                    latest, GetForceXidFromGTM(), IsInitProcessingMode(), IsConnFromCoord()); 
        }
                    
        GetGlobalTimestampFromGTM(snapshot);

        return;
    } 

    return;

}

static void
GetGlobalTimestampFromGTM(Snapshot snapshot)
{
    GlobalTimestamp start_ts;

    start_ts = (GlobalTimestamp) GetGlobalTimestampGTM();
    snapshot->start_ts = start_ts;
    
    if (!GlobalTimestampIsValid(start_ts))
    {
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("GTM error, could not obtain global timestamp. Current XID = %d, Autovac = %d", 
                             GetTopTransactionIdIfAny(), IsAutoVacuumWorkerProcess())));
    }
    if(enable_distri_print)
    {
        elog(LOG, "Get GTM global timestamp " INT64_FORMAT, snapshot->start_ts);
    }


}



static void
GetGlobalTimestampFromGlobalSnapshot(Snapshot snapshot)
{
    if ((SNAPSHOT_COORDINATOR == globalSnapshot.snapshot_source  
                && GlobalTimestampIsValid(globalSnapshot.gts)))
    {
        snapshot->start_ts = globalSnapshot.gts;

        /* if gts is local, use local snapshot */
        if (CommitTimestampIsLocal(snapshot->start_ts))
        {
            snapshot->local = true;
        }

        if(enable_distri_print)
        {
            elog(LOG, "Get global timestamp from global " INT64_FORMAT, snapshot->start_ts);
        }
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("could not obtain global timestamp for snapshot. Current XID = %d, Autovac = %d", 
                             GetTopTransactionIdIfAny(), IsAutoVacuumWorkerProcess())));
    }
}
#endif

#if 0
static void
GetSnapshotDataFromGTM(Snapshot snapshot)
{// #lizard forgives
    GTM_Snapshot gtm_snapshot;
    GlobalTransactionId reporting_xmin;
    bool canbe_grouped = (!FirstSnapshotSet) || (!IsolationUsesXactSnapshot());
    bool xmin_changed = false;

    /*
     * We never want to use a snapshot whose xmin is older than the
     * RecentGlobalXmin computed by the GTM. While it does not look likely that
     * that this will ever happen because both these computations happen on the
     * GTM, we are still worried about a race condition where a backend sends a
     * snapshot request, and before snapshot is received, the cluster monitor
     * reports our Xmin (which obviously does not include this snapshot's
     * xmin). Now if GTM processes the snapshot request first, computes
     * snapshot's xmin and then receives our Xmin-report, it may actually moves
     * RecentGlobalXmin beyond snapshot's xmin assuming some transactions
     * finished in between.
     *
     * We try to introduce some interlock between the Xmin reporting and
     * snapshot request. Since we don't want to wait on a lock while Xmin is
     * being reported by the cluster monitor process, we just make sure that
     * the snapshot's xmin is not older than the Xmin we are currently
     * reporting. Given that this is a very rare possibility, we just get a
     * fresh snapshot from the GTM.
     *
     */
    
    LWLockAcquire(ClusterMonitorLock, LW_SHARED);

retry:
    reporting_xmin = ClusterMonitorGetReportingGlobalXmin();
    
    xmin_changed = false;
    if (TransactionIdIsValid(reporting_xmin) &&
        !TransactionIdIsValid(MyPgXact->xmin))
    {
        MyPgXact->xmin = reporting_xmin;
        xmin_changed = true;
    }

    gtm_snapshot = GetSnapshotGTM(GetCurrentTransactionIdIfAny(), canbe_grouped);

    if (!gtm_snapshot)
        ereport(ERROR,
                (errcode(ERRCODE_CONNECTION_FAILURE),
                 errmsg("GTM error, could not obtain snapshot. Current XID = %d, Autovac = %d", GetCurrentTransactionId(), IsAutoVacuumWorkerProcess())));
    else
    {
        if (xmin_changed)
            MyPgXact->xmin = InvalidTransactionId;
        if (TransactionIdPrecedes(gtm_snapshot->sn_xmin, reporting_xmin))
            goto retry;

        /* 
         * Set RecentGlobalXmin by copying from the shared memory state
         * maintained by the Clutser Monitor
         */
        RecentGlobalXmin = ClusterMonitorGetGlobalXmin();
        if (!TransactionIdIsValid(RecentGlobalXmin))
            RecentGlobalXmin = FirstNormalTransactionId;
        /*
         * XXX Is it ok to set RecentGlobalDataXmin same as RecentGlobalXmin ?
         */
        RecentGlobalDataXmin = RecentGlobalXmin;
        SetGlobalSnapshotData(gtm_snapshot->sn_xmin, gtm_snapshot->sn_xmax,
                gtm_snapshot->sn_xcnt, gtm_snapshot->sn_xip, SNAPSHOT_DIRECT);
        GetSnapshotFromGlobalSnapshot(snapshot);
    }
    LWLockRelease(ClusterMonitorLock);
}

static void
GetSnapshotFromGlobalSnapshot(Snapshot snapshot)
{// #lizard forgives
    if ((globalSnapshot.snapshot_source == SNAPSHOT_COORDINATOR ||
                globalSnapshot.snapshot_source == SNAPSHOT_DIRECT)
                && TransactionIdIsValid(globalSnapshot.gxmin))
    {
        TransactionId global_xmin;

        snapshot->xmin = globalSnapshot.gxmin;
        snapshot->xmax = globalSnapshot.gxmax;
        snapshot->xcnt = globalSnapshot.gxcnt;
        /*
         * Allocating space for maxProcs xids is usually overkill; numProcs would
         * be sufficient.  But it seems better to do the malloc while not holding
         * the lock, so we can't look at numProcs.  Likewise, we allocate much
         * more subxip storage than is probably needed.
         *
         * This does open a possibility for avoiding repeated malloc/free: since
         * maxProcs does not change at runtime, we can simply reuse the previous
         * xip arrays if any.  (This relies on the fact that all callers pass
         * static SnapshotData structs.) */
        if (snapshot->xip == NULL)
        {
            ProcArrayStruct *arrayP = procArray;
            /*
             * First call for this snapshot
             */
            snapshot->xip = (TransactionId *)
                malloc(Max(arrayP->maxProcs, globalSnapshot.gxcnt) * sizeof(TransactionId));
            if (snapshot->xip == NULL)
                ereport(ERROR,
                        (errcode(ERRCODE_OUT_OF_MEMORY),
                         errmsg("out of memory")));
            snapshot->max_xcnt = Max(arrayP->maxProcs, globalSnapshot.gxcnt);

            Assert(snapshot->subxip == NULL);
            snapshot->subxip = (TransactionId *)
                malloc(arrayP->maxProcs * PGPROC_MAX_CACHED_SUBXIDS * sizeof(TransactionId));
            if (snapshot->subxip == NULL)
                ereport(ERROR,
                        (errcode(ERRCODE_OUT_OF_MEMORY),
                         errmsg("out of memory")));
        }
        else if (snapshot->max_xcnt < globalSnapshot.gxcnt)
        {
            snapshot->xip = (TransactionId *)
                realloc(snapshot->xip, globalSnapshot.gxcnt * sizeof(TransactionId));
            if (snapshot->xip == NULL)
                ereport(ERROR,
                        (errcode(ERRCODE_OUT_OF_MEMORY),
                         errmsg("out of memory")));
            snapshot->max_xcnt = globalSnapshot.gxcnt;
        }

        /* PGXCTODO - set this until we handle subtransactions. */
        snapshot->subxcnt = 0;

        /*
         * This is a new snapshot, so set both refcounts are zero, and mark it
         * as not copied in persistent memory.
         */
        snapshot->active_count = 0;
        snapshot->regd_count = 0;
        snapshot->copied = false;

        /*
         * Start of handling for local ANALYZE
         * Make adjustments for any running auto ANALYZE commands
         */
        LWLockAcquire(ProcArrayLock, LW_SHARED);

        /*
         * Once we have a SHARED lock on the ProcArrayLock, fetch the
         * GlobalXmin and ensure that the snapshot we are dealing with isn't
         * too old. Since such a snapshot may need to see rows that have
         * already been removed by the server
         *
         * These scenarios are not very likely to happen because the
         * ClusterMonitor will ensure that GlobalXmins are reported to GTM in
         * time and the GlobalXmin on the GTM can't advance past the reported
         * xmins. But in some cases where a node fails to report its GlobalXmin
         * and gets excluded from the list of nodes on GTM, the GlobalXmin will
         * be advanced. Usually such node will shoot itself in the head
         * and rejoin the cluster, but if at all it sends a snapshot to us, we
         * should protect ourselves from using it
         */
        global_xmin = ClusterMonitorGetGlobalXmin();
        if (!TransactionIdIsValid(global_xmin))
            global_xmin = FirstNormalTransactionId;

        if (TransactionIdPrecedes(globalSnapshot.gxmin, global_xmin))
            elog(ERROR, "Snapshot too old - RecentGlobalXmin (%d) has already "
                    "advanced past the snapshot xmin (%d)",
                    global_xmin, globalSnapshot.gxmin);

        memcpy(snapshot->xip, globalSnapshot.gxip,
                globalSnapshot.gxcnt * sizeof(TransactionId));
        snapshot->curcid = GetCurrentCommandId(false);

        if (!TransactionIdIsValid(MyPgXact->xmin))
            MyPgXact->xmin = TransactionXmin = globalSnapshot.gxmin;

        RecentXmin = globalSnapshot.gxmin;
        RecentGlobalXmin = global_xmin;

        /*
         * XXX Is it ok to set RecentGlobalDataXmin same as RecentGlobalXmin ?
         */
        RecentGlobalDataXmin = RecentGlobalXmin;

        if (!TransactionIdIsValid(MyPgXact->xmin))
            MyPgXact->xmin = snapshot->xmin;

        LWLockRelease(ProcArrayLock);
        /* End handling of local analyze XID in snapshots */
    }
    else
        elog(ERROR, "Cannot set snapshot from global snapshot");
}
#endif

#endif /* PGXC */

/* ----------------------------------------------
 *        KnownAssignedTransactions sub-module
 * ----------------------------------------------
 */

/*
 * In Hot Standby mode, we maintain a list of transactions that are (or were)
 * running in the master at the current point in WAL.  These XIDs must be
 * treated as running by standby transactions, even though they are not in
 * the standby server's PGXACT array.
 *
 * We record all XIDs that we know have been assigned.  That includes all the
 * XIDs seen in WAL records, plus all unobserved XIDs that we can deduce have
 * been assigned.  We can deduce the existence of unobserved XIDs because we
 * know XIDs are assigned in sequence, with no gaps.  The KnownAssignedXids
 * list expands as new XIDs are observed or inferred, and contracts when
 * transaction completion records arrive.
 *
 * During hot standby we do not fret too much about the distinction between
 * top-level XIDs and subtransaction XIDs. We store both together in the
 * KnownAssignedXids list.  In backends, this is copied into snapshots in
 * GetSnapshotData(), taking advantage of the fact that XidInMVCCSnapshot()
 * doesn't care about the distinction either.  Subtransaction XIDs are
 * effectively treated as top-level XIDs and in the typical case pg_subtrans
 * links are *not* maintained (which does not affect visibility).
 *
 * We have room in KnownAssignedXids and in snapshots to hold maxProcs *
 * (1 + PGPROC_MAX_CACHED_SUBXIDS) XIDs, so every master transaction must
 * report its subtransaction XIDs in a WAL XLOG_XACT_ASSIGNMENT record at
 * least every PGPROC_MAX_CACHED_SUBXIDS.  When we receive one of these
 * records, we mark the subXIDs as children of the top XID in pg_subtrans,
 * and then remove them from KnownAssignedXids.  This prevents overflow of
 * KnownAssignedXids and snapshots, at the cost that status checks for these
 * subXIDs will take a slower path through TransactionIdIsInProgress().
 * This means that KnownAssignedXids is not necessarily complete for subXIDs,
 * though it should be complete for top-level XIDs; this is the same situation
 * that holds with respect to the PGPROC entries in normal running.
 *
 * When we throw away subXIDs from KnownAssignedXids, we need to keep track of
 * that, similarly to tracking overflow of a PGPROC's subxids array.  We do
 * that by remembering the lastOverflowedXID, ie the last thrown-away subXID.
 * As long as that is within the range of interesting XIDs, we have to assume
 * that subXIDs are missing from snapshots.  (Note that subXID overflow occurs
 * on primary when 65th subXID arrives, whereas on standby it occurs when 64th
 * subXID arrives - that is not an error.)
 *
 * Should a backend on primary somehow disappear before it can write an abort
 * record, then we just leave those XIDs in KnownAssignedXids. They actually
 * aborted but we think they were running; the distinction is irrelevant
 * because either way any changes done by the transaction are not visible to
 * backends in the standby.  We prune KnownAssignedXids when
 * XLOG_RUNNING_XACTS arrives, to forestall possible overflow of the
 * array due to such dead XIDs.
 */

/*
 * RecordKnownAssignedTransactionIds
 *        Record the given XID in KnownAssignedXids, as well as any preceding
 *        unobserved XIDs.
 *
 * RecordKnownAssignedTransactionIds() should be run for *every* WAL record
 * associated with a transaction. Must be called for each record after we
 * have executed StartupCLOG() et al, since we must ExtendCLOG() etc..
 *
 * Called during recovery in analogy with and in place of GetNewTransactionId()
 */
void
RecordKnownAssignedTransactionIds(TransactionId xid)
{
    Assert(standbyState >= STANDBY_INITIALIZED);
    Assert(TransactionIdIsValid(xid));
    Assert(TransactionIdIsValid(latestObservedXid));

    elog(trace_recovery(DEBUG4), "record known xact %u latestObservedXid %u",
         xid, latestObservedXid);

    /*
     * When a newly observed xid arrives, it is frequently the case that it is
     * *not* the next xid in sequence. When this occurs, we must treat the
     * intervening xids as running also.
     */
    if (TransactionIdFollows(xid, latestObservedXid))
    {
        TransactionId next_expected_xid;

        /*
         * Extend subtrans like we do in GetNewTransactionId() during normal
         * operation using individual extend steps. Note that we do not need
         * to extend clog since its extensions are WAL logged.
         *
         * This part has to be done regardless of standbyState since we
         * immediately start assigning subtransactions to their toplevel
         * transactions.
         */
        next_expected_xid = latestObservedXid;
        while (TransactionIdPrecedes(next_expected_xid, xid))
        {
            TransactionIdAdvance(next_expected_xid);
            ExtendSUBTRANS(next_expected_xid);
        }
        Assert(next_expected_xid == xid);

        /*
         * If the KnownAssignedXids machinery isn't up yet, there's nothing
         * more to do since we don't track assigned xids yet.
         */
        if (standbyState <= STANDBY_INITIALIZED)
        {
            latestObservedXid = xid;
            return;
        }

        /*
         * Add (latestObservedXid, xid] onto the KnownAssignedXids array.
         */
        next_expected_xid = latestObservedXid;
        TransactionIdAdvance(next_expected_xid);
        KnownAssignedXidsAdd(next_expected_xid, xid, false);

        /*
         * Now we can advance latestObservedXid
         */
        latestObservedXid = xid;

        /* ShmemVariableCache->nextXid must be beyond any observed xid */
        next_expected_xid = latestObservedXid;
        TransactionIdAdvance(next_expected_xid);
        LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
        ShmemVariableCache->nextXid = next_expected_xid;
        LWLockRelease(XidGenLock);
    }
}

/*
 * ExpireTreeKnownAssignedTransactionIds
 *        Remove the given XIDs from KnownAssignedXids.
 *
 * Called during recovery in analogy with and in place of ProcArrayEndTransaction()
 */
void
ExpireTreeKnownAssignedTransactionIds(TransactionId xid, int nsubxids,
                                      TransactionId *subxids, TransactionId max_xid)
{
    Assert(standbyState >= STANDBY_INITIALIZED);

    /*
     * Uses same locking as transaction commit
     */
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    KnownAssignedXidsRemoveTree(xid, nsubxids, subxids);

    /* As in ProcArrayEndTransaction, advance latestCompletedXid */
    if (TransactionIdPrecedes(ShmemVariableCache->latestCompletedXid,
                              max_xid))
        ShmemVariableCache->latestCompletedXid = max_xid;

    LWLockRelease(ProcArrayLock);
}

/*
 * ExpireAllKnownAssignedTransactionIds
 *        Remove all entries in KnownAssignedXids
 */
void
ExpireAllKnownAssignedTransactionIds(void)
{
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
    KnownAssignedXidsRemovePreceding(InvalidTransactionId);
    LWLockRelease(ProcArrayLock);
}

/*
 * ExpireOldKnownAssignedTransactionIds
 *        Remove KnownAssignedXids entries preceding the given XID
 */
void
ExpireOldKnownAssignedTransactionIds(TransactionId xid)
{
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
    KnownAssignedXidsRemovePreceding(xid);
    LWLockRelease(ProcArrayLock);
}


/*
 * Private module functions to manipulate KnownAssignedXids
 *
 * There are 5 main uses of the KnownAssignedXids data structure:
 *
 *    * backends taking snapshots - all valid XIDs need to be copied out
 *    * backends seeking to determine presence of a specific XID
 *    * startup process adding new known-assigned XIDs
 *    * startup process removing specific XIDs as transactions end
 *    * startup process pruning array when special WAL records arrive
 *
 * This data structure is known to be a hot spot during Hot Standby, so we
 * go to some lengths to make these operations as efficient and as concurrent
 * as possible.
 *
 * The XIDs are stored in an array in sorted order --- TransactionIdPrecedes
 * order, to be exact --- to allow binary search for specific XIDs.  Note:
 * in general TransactionIdPrecedes would not provide a total order, but
 * we know that the entries present at any instant should not extend across
 * a large enough fraction of XID space to wrap around (the master would
 * shut down for fear of XID wrap long before that happens).  So it's OK to
 * use TransactionIdPrecedes as a binary-search comparator.
 *
 * It's cheap to maintain the sortedness during insertions, since new known
 * XIDs are always reported in XID order; we just append them at the right.
 *
 * To keep individual deletions cheap, we need to allow gaps in the array.
 * This is implemented by marking array elements as valid or invalid using
 * the parallel boolean array KnownAssignedXidsValid[].  A deletion is done
 * by setting KnownAssignedXidsValid[i] to false, *without* clearing the
 * XID entry itself.  This preserves the property that the XID entries are
 * sorted, so we can do binary searches easily.  Periodically we compress
 * out the unused entries; that's much cheaper than having to compress the
 * array immediately on every deletion.
 *
 * The actually valid items in KnownAssignedXids[] and KnownAssignedXidsValid[]
 * are those with indexes tail <= i < head; items outside this subscript range
 * have unspecified contents.  When head reaches the end of the array, we
 * force compression of unused entries rather than wrapping around, since
 * allowing wraparound would greatly complicate the search logic.  We maintain
 * an explicit tail pointer so that pruning of old XIDs can be done without
 * immediately moving the array contents.  In most cases only a small fraction
 * of the array contains valid entries at any instant.
 *
 * Although only the startup process can ever change the KnownAssignedXids
 * data structure, we still need interlocking so that standby backends will
 * not observe invalid intermediate states.  The convention is that backends
 * must hold shared ProcArrayLock to examine the array.  To remove XIDs from
 * the array, the startup process must hold ProcArrayLock exclusively, for
 * the usual transactional reasons (compare commit/abort of a transaction
 * during normal running).  Compressing unused entries out of the array
 * likewise requires exclusive lock.  To add XIDs to the array, we just insert
 * them into slots to the right of the head pointer and then advance the head
 * pointer.  This wouldn't require any lock at all, except that on machines
 * with weak memory ordering we need to be careful that other processors
 * see the array element changes before they see the head pointer change.
 * We handle this by using a spinlock to protect reads and writes of the
 * head/tail pointers.  (We could dispense with the spinlock if we were to
 * create suitable memory access barrier primitives and use those instead.)
 * The spinlock must be taken to read or write the head/tail pointers unless
 * the caller holds ProcArrayLock exclusively.
 *
 * Algorithmic analysis:
 *
 * If we have a maximum of M slots, with N XIDs currently spread across
 * S elements then we have N <= S <= M always.
 *
 *    * Adding a new XID is O(1) and needs little locking (unless compression
 *        must happen)
 *    * Compressing the array is O(S) and requires exclusive lock
 *    * Removing an XID is O(logS) and requires exclusive lock
 *    * Taking a snapshot is O(S) and requires shared lock
 *    * Checking for an XID is O(logS) and requires shared lock
 *
 * In comparison, using a hash table for KnownAssignedXids would mean that
 * taking snapshots would be O(M). If we can maintain S << M then the
 * sorted array technique will deliver significantly faster snapshots.
 * If we try to keep S too small then we will spend too much time compressing,
 * so there is an optimal point for any workload mix. We use a heuristic to
 * decide when to compress the array, though trimming also helps reduce
 * frequency of compressing. The heuristic requires us to track the number of
 * currently valid XIDs in the array.
 */


/*
 * Compress KnownAssignedXids by shifting valid data down to the start of the
 * array, removing any gaps.
 *
 * A compression step is forced if "force" is true, otherwise we do it
 * only if a heuristic indicates it's a good time to do it.
 *
 * Caller must hold ProcArrayLock in exclusive mode.
 */
static void
KnownAssignedXidsCompress(bool force)
{
    /* use volatile pointer to prevent code rearrangement */
    volatile ProcArrayStruct *pArray = procArray;
    int            head,
                tail;
    int            compress_index;
    int            i;

    /* no spinlock required since we hold ProcArrayLock exclusively */
    head = pArray->headKnownAssignedXids;
    tail = pArray->tailKnownAssignedXids;

    if (!force)
    {
        /*
         * If we can choose how much to compress, use a heuristic to avoid
         * compressing too often or not often enough.
         *
         * Heuristic is if we have a large enough current spread and less than
         * 50% of the elements are currently in use, then compress. This
         * should ensure we compress fairly infrequently. We could compress
         * less often though the virtual array would spread out more and
         * snapshots would become more expensive.
         */
        int            nelements = head - tail;

        if (nelements < 4 * PROCARRAY_MAXPROCS ||
            nelements < 2 * pArray->numKnownAssignedXids)
            return;
    }

    /*
     * We compress the array by reading the valid values from tail to head,
     * re-aligning data to 0th element.
     */
    compress_index = 0;
    for (i = tail; i < head; i++)
    {
        if (KnownAssignedXidsValid[i])
        {
            KnownAssignedXids[compress_index] = KnownAssignedXids[i];
            KnownAssignedXidsValid[compress_index] = true;
            compress_index++;
        }
    }

    pArray->tailKnownAssignedXids = 0;
    pArray->headKnownAssignedXids = compress_index;
}

/*
 * Add xids into KnownAssignedXids at the head of the array.
 *
 * xids from from_xid to to_xid, inclusive, are added to the array.
 *
 * If exclusive_lock is true then caller already holds ProcArrayLock in
 * exclusive mode, so we need no extra locking here.  Else caller holds no
 * lock, so we need to be sure we maintain sufficient interlocks against
 * concurrent readers.  (Only the startup process ever calls this, so no need
 * to worry about concurrent writers.)
 */
static void
KnownAssignedXidsAdd(TransactionId from_xid, TransactionId to_xid,
                     bool exclusive_lock)
{// #lizard forgives
    /* use volatile pointer to prevent code rearrangement */
    volatile ProcArrayStruct *pArray = procArray;
    TransactionId next_xid;
    int            head,
                tail;
    int            nxids;
    int            i;

    Assert(TransactionIdPrecedesOrEquals(from_xid, to_xid));

    /*
     * Calculate how many array slots we'll need.  Normally this is cheap; in
     * the unusual case where the XIDs cross the wrap point, we do it the hard
     * way.
     */
    if (to_xid >= from_xid)
        nxids = to_xid - from_xid + 1;
    else
    {
        nxids = 1;
        next_xid = from_xid;
        while (TransactionIdPrecedes(next_xid, to_xid))
        {
            nxids++;
            TransactionIdAdvance(next_xid);
        }
    }

    /*
     * Since only the startup process modifies the head/tail pointers, we
     * don't need a lock to read them here.
     */
    head = pArray->headKnownAssignedXids;
    tail = pArray->tailKnownAssignedXids;

    Assert(head >= 0 && head <= pArray->maxKnownAssignedXids);
    Assert(tail >= 0 && tail < pArray->maxKnownAssignedXids);

    /*
     * Verify that insertions occur in TransactionId sequence.  Note that even
     * if the last existing element is marked invalid, it must still have a
     * correctly sequenced XID value.
     */
    if (head > tail &&
        TransactionIdFollowsOrEquals(KnownAssignedXids[head - 1], from_xid))
    {
        KnownAssignedXidsDisplay(LOG);
        elog(ERROR, "out-of-order XID insertion in KnownAssignedXids");
    }

    /*
     * If our xids won't fit in the remaining space, compress out free space
     */
    if (head + nxids > pArray->maxKnownAssignedXids)
    {
        /* must hold lock to compress */
        if (!exclusive_lock)
            LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

        KnownAssignedXidsCompress(true);

        head = pArray->headKnownAssignedXids;
        /* note: we no longer care about the tail pointer */

        if (!exclusive_lock)
            LWLockRelease(ProcArrayLock);

        /*
         * If it still won't fit then we're out of memory
         */
        if (head + nxids > pArray->maxKnownAssignedXids)
            elog(ERROR, "too many KnownAssignedXids");
    }

    /* Now we can insert the xids into the space starting at head */
    next_xid = from_xid;
    for (i = 0; i < nxids; i++)
    {
        KnownAssignedXids[head] = next_xid;
        KnownAssignedXidsValid[head] = true;
        TransactionIdAdvance(next_xid);
        head++;
    }

    /* Adjust count of number of valid entries */
    pArray->numKnownAssignedXids += nxids;

    /*
     * Now update the head pointer.  We use a spinlock to protect this
     * pointer, not because the update is likely to be non-atomic, but to
     * ensure that other processors see the above array updates before they
     * see the head pointer change.
     *
     * If we're holding ProcArrayLock exclusively, there's no need to take the
     * spinlock.
     */
    if (exclusive_lock)
        pArray->headKnownAssignedXids = head;
    else
    {
        SpinLockAcquire(&pArray->known_assigned_xids_lck);
        pArray->headKnownAssignedXids = head;
        SpinLockRelease(&pArray->known_assigned_xids_lck);
    }
}

/*
 * KnownAssignedXidsSearch
 *
 * Searches KnownAssignedXids for a specific xid and optionally removes it.
 * Returns true if it was found, false if not.
 *
 * Caller must hold ProcArrayLock in shared or exclusive mode.
 * Exclusive lock must be held for remove = true.
 */
static bool
KnownAssignedXidsSearch(TransactionId xid, bool remove)
{// #lizard forgives
    /* use volatile pointer to prevent code rearrangement */
    volatile ProcArrayStruct *pArray = procArray;
    int            first,
                last;
    int            head;
    int            tail;
    int            result_index = -1;

    if (remove)
    {
        /* we hold ProcArrayLock exclusively, so no need for spinlock */
        tail = pArray->tailKnownAssignedXids;
        head = pArray->headKnownAssignedXids;
    }
    else
    {
        /* take spinlock to ensure we see up-to-date array contents */
        SpinLockAcquire(&pArray->known_assigned_xids_lck);
        tail = pArray->tailKnownAssignedXids;
        head = pArray->headKnownAssignedXids;
        SpinLockRelease(&pArray->known_assigned_xids_lck);
    }

    /*
     * Standard binary search.  Note we can ignore the KnownAssignedXidsValid
     * array here, since even invalid entries will contain sorted XIDs.
     */
    first = tail;
    last = head - 1;
    while (first <= last)
    {
        int            mid_index;
        TransactionId mid_xid;

        mid_index = (first + last) / 2;
        mid_xid = KnownAssignedXids[mid_index];

        if (xid == mid_xid)
        {
            result_index = mid_index;
            break;
        }
        else if (TransactionIdPrecedes(xid, mid_xid))
            last = mid_index - 1;
        else
            first = mid_index + 1;
    }

    if (result_index < 0)
        return false;            /* not in array */

    if (!KnownAssignedXidsValid[result_index])
        return false;            /* in array, but invalid */

    if (remove)
    {
        KnownAssignedXidsValid[result_index] = false;

        pArray->numKnownAssignedXids--;
        Assert(pArray->numKnownAssignedXids >= 0);

        /*
         * If we're removing the tail element then advance tail pointer over
         * any invalid elements.  This will speed future searches.
         */
        if (result_index == tail)
        {
            tail++;
            while (tail < head && !KnownAssignedXidsValid[tail])
                tail++;
            if (tail >= head)
            {
                /* Array is empty, so we can reset both pointers */
                pArray->headKnownAssignedXids = 0;
                pArray->tailKnownAssignedXids = 0;
            }
            else
            {
                pArray->tailKnownAssignedXids = tail;
            }
        }
    }

    return true;
}

/*
 * Is the specified XID present in KnownAssignedXids[]?
 *
 * Caller must hold ProcArrayLock in shared or exclusive mode.
 */
static bool
KnownAssignedXidExists(TransactionId xid)
{
    Assert(TransactionIdIsValid(xid));

    return KnownAssignedXidsSearch(xid, false);
}

/*
 * Remove the specified XID from KnownAssignedXids[].
 *
 * Caller must hold ProcArrayLock in exclusive mode.
 */
static void
KnownAssignedXidsRemove(TransactionId xid)
{
    Assert(TransactionIdIsValid(xid));

    elog(trace_recovery(DEBUG4), "remove KnownAssignedXid %u", xid);

    /*
     * Note: we cannot consider it an error to remove an XID that's not
     * present.  We intentionally remove subxact IDs while processing
     * XLOG_XACT_ASSIGNMENT, to avoid array overflow.  Then those XIDs will be
     * removed again when the top-level xact commits or aborts.
     *
     * It might be possible to track such XIDs to distinguish this case from
     * actual errors, but it would be complicated and probably not worth it.
     * So, just ignore the search result.
     */
    (void) KnownAssignedXidsSearch(xid, true);
}

/*
 * KnownAssignedXidsRemoveTree
 *        Remove xid (if it's not InvalidTransactionId) and all the subxids.
 *
 * Caller must hold ProcArrayLock in exclusive mode.
 */
static void
KnownAssignedXidsRemoveTree(TransactionId xid, int nsubxids,
                            TransactionId *subxids)
{
    int            i;

    if (TransactionIdIsValid(xid))
        KnownAssignedXidsRemove(xid);

    for (i = 0; i < nsubxids; i++)
        KnownAssignedXidsRemove(subxids[i]);

    /* Opportunistically compress the array */
    KnownAssignedXidsCompress(false);
}

/*
 * Prune KnownAssignedXids up to, but *not* including xid. If xid is invalid
 * then clear the whole table.
 *
 * Caller must hold ProcArrayLock in exclusive mode.
 */
static void
KnownAssignedXidsRemovePreceding(TransactionId removeXid)
{// #lizard forgives
    /* use volatile pointer to prevent code rearrangement */
    volatile ProcArrayStruct *pArray = procArray;
    int            count = 0;
    int            head,
                tail,
                i;

    if (!TransactionIdIsValid(removeXid))
    {
        elog(trace_recovery(DEBUG4), "removing all KnownAssignedXids");
        pArray->numKnownAssignedXids = 0;
        pArray->headKnownAssignedXids = pArray->tailKnownAssignedXids = 0;
        return;
    }

    elog(trace_recovery(DEBUG4), "prune KnownAssignedXids to %u", removeXid);

    /*
     * Mark entries invalid starting at the tail.  Since array is sorted, we
     * can stop as soon as we reach an entry >= removeXid.
     */
    tail = pArray->tailKnownAssignedXids;
    head = pArray->headKnownAssignedXids;

    for (i = tail; i < head; i++)
    {
        if (KnownAssignedXidsValid[i])
        {
            TransactionId knownXid = KnownAssignedXids[i];

            if (TransactionIdFollowsOrEquals(knownXid, removeXid))
                break;

            if (!StandbyTransactionIdIsPrepared(knownXid))
            {
                KnownAssignedXidsValid[i] = false;
                count++;
            }
        }
    }

    pArray->numKnownAssignedXids -= count;
    Assert(pArray->numKnownAssignedXids >= 0);

    /*
     * Advance the tail pointer if we've marked the tail item invalid.
     */
    for (i = tail; i < head; i++)
    {
        if (KnownAssignedXidsValid[i])
            break;
    }
    if (i >= head)
    {
        /* Array is empty, so we can reset both pointers */
        pArray->headKnownAssignedXids = 0;
        pArray->tailKnownAssignedXids = 0;
    }
    else
    {
        pArray->tailKnownAssignedXids = i;
    }

    /* Opportunistically compress the array */
    KnownAssignedXidsCompress(false);
}

/*
 * KnownAssignedXidsGet - Get an array of xids by scanning KnownAssignedXids.
 * We filter out anything >= xmax.
 *
 * Returns the number of XIDs stored into xarray[].  Caller is responsible
 * that array is large enough.
 *
 * Caller must hold ProcArrayLock in (at least) shared mode.
 */
static int
KnownAssignedXidsGet(TransactionId *xarray, TransactionId xmax)
{
    TransactionId xtmp = InvalidTransactionId;

    return KnownAssignedXidsGetAndSetXmin(xarray, &xtmp, xmax);
}

/*
 * KnownAssignedXidsGetAndSetXmin - as KnownAssignedXidsGet, plus
 * we reduce *xmin to the lowest xid value seen if not already lower.
 *
 * Caller must hold ProcArrayLock in (at least) shared mode.
 */
static int
KnownAssignedXidsGetAndSetXmin(TransactionId *xarray, TransactionId *xmin,
                               TransactionId xmax)
{
    int            count = 0;
    int            head,
                tail;
    int            i;

    /*
     * Fetch head just once, since it may change while we loop. We can stop
     * once we reach the initially seen head, since we are certain that an xid
     * cannot enter and then leave the array while we hold ProcArrayLock.  We
     * might miss newly-added xids, but they should be >= xmax so irrelevant
     * anyway.
     *
     * Must take spinlock to ensure we see up-to-date array contents.
     */
    SpinLockAcquire(&procArray->known_assigned_xids_lck);
    tail = procArray->tailKnownAssignedXids;
    head = procArray->headKnownAssignedXids;
    SpinLockRelease(&procArray->known_assigned_xids_lck);

    for (i = tail; i < head; i++)
    {
        /* Skip any gaps in the array */
        if (KnownAssignedXidsValid[i])
        {
            TransactionId knownXid = KnownAssignedXids[i];

            /*
             * Update xmin if required.  Only the first XID need be checked,
             * since the array is sorted.
             */
            if (count == 0 &&
                TransactionIdPrecedes(knownXid, *xmin))
                *xmin = knownXid;

            /*
             * Filter out anything >= xmax, again relying on sorted property
             * of array.
             */
            if (TransactionIdIsValid(xmax) &&
                TransactionIdFollowsOrEquals(knownXid, xmax))
                break;

            /* Add knownXid into output array */
            xarray[count++] = knownXid;
        }
    }

    return count;
}

/*
 * Get oldest XID in the KnownAssignedXids array, or InvalidTransactionId
 * if nothing there.
 */
static TransactionId
KnownAssignedXidsGetOldestXmin(void)
{
    int            head,
                tail;
    int            i;

    /*
     * Fetch head just once, since it may change while we loop.
     */
    SpinLockAcquire(&procArray->known_assigned_xids_lck);
    tail = procArray->tailKnownAssignedXids;
    head = procArray->headKnownAssignedXids;
    SpinLockRelease(&procArray->known_assigned_xids_lck);

    for (i = tail; i < head; i++)
    {
        /* Skip any gaps in the array */
        if (KnownAssignedXidsValid[i])
            return KnownAssignedXids[i];
    }

    return InvalidTransactionId;
}

/*
 * Display KnownAssignedXids to provide debug trail
 *
 * Currently this is only called within startup process, so we need no
 * special locking.
 *
 * Note this is pretty expensive, and much of the expense will be incurred
 * even if the elog message will get discarded.  It's not currently called
 * in any performance-critical places, however, so no need to be tenser.
 */
static void
KnownAssignedXidsDisplay(int trace_level)
{
    /* use volatile pointer to prevent code rearrangement */
    volatile ProcArrayStruct *pArray = procArray;
    StringInfoData buf;
    int            head,
                tail,
                i;
    int            nxids = 0;

    tail = pArray->tailKnownAssignedXids;
    head = pArray->headKnownAssignedXids;

    initStringInfo(&buf);

    for (i = tail; i < head; i++)
    {
        if (KnownAssignedXidsValid[i])
        {
            nxids++;
            appendStringInfo(&buf, "[%d]=%u ", i, KnownAssignedXids[i]);
        }
    }

    elog(trace_level, "%d KnownAssignedXids (num=%d tail=%d head=%d) %s",
         nxids,
         pArray->numKnownAssignedXids,
         pArray->tailKnownAssignedXids,
         pArray->headKnownAssignedXids,
         buf.data);

    pfree(buf.data);
}


#ifdef XCP
/*
 * GetGlobalSessionInfo
 *
 * Determine the global session id of the specified backend process
 * Returns coordinator node_id and pid of the initiating coordinator session.
 * If no such backend or global session id is not defined for the backend
 * return zero values.
 */
void
GetGlobalSessionInfo(int pid, Oid *coordId, int *coordPid)
{
    ProcArrayStruct *arrayP = procArray;
    int            index;

    *coordId = InvalidOid;
    *coordPid = 0;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    /*
     * Scan processes and get from it info about the parent session
     */
    for (index = 0; index < arrayP->numProcs; index++)
    {
        volatile PGPROC *proc = &allProcs[arrayP->pgprocnos[index]];

        if (proc->pid == pid)
        {
            *coordId = proc->coordId;
            *coordPid = proc->coordPid;
            break;
        }
    }

    LWLockRelease(ProcArrayLock);
}


/*
 * GetFirstBackendId
 *
 * Determine BackendId of the current process.
 * The caller must hold the ProcArrayLock and the global session id should
 * be defined.
 */
int
GetFirstBackendId(int *numBackends, int *backends)
{// #lizard forgives
    ProcArrayStruct *arrayP = procArray;
    Oid                coordId = MyProc->coordId;
    int                coordPid = MyProc->coordPid;
    int                bCount = 0;
    int                bPids[MaxBackends];
    int                index;

    Assert(OidIsValid(coordId));

    /* Scan processes */
    for (index = 0; index < arrayP->numProcs; index++)
    {
        volatile PGPROC *proc = &allProcs[arrayP->pgprocnos[index]];

        /* Skip MyProc */
        if (proc == MyProc)
            continue;

        if (proc->coordId == coordId && proc->coordPid == coordPid)
        {
            /* BackendId is the same for all backends of the session */
            if (proc->firstBackendId != InvalidBackendId)
                return proc->firstBackendId;

            bPids[bCount++] = proc->pid;
        }
    }

    if (*numBackends > 0)
    {
        int i, j;
        /*
         * This is not the first invocation, to prevent endless loop in case
         * if first backend failed to complete initialization check if all the
         * processes which were intially found are still here, throw error if
         * not.
         */
        for (i = 0; i < *numBackends; i++)
        {
            bool found = false;

            for (j = 0; j < bCount; j++)
            {
                if (bPids[j] == backends[i])
                {
                    found = true;
                    break;
                }
            }

            if (!found)
                elog(ERROR, "Failed to determine BackendId for distributed session");
        }
    }
    else
    {
        *numBackends = bCount;
        if (bCount)
            memcpy(backends, bPids, bCount * sizeof(int));
    }
    return InvalidBackendId;
}
#endif

/*
 * KnownAssignedXidsReset
 *        Resets KnownAssignedXids to be empty
 */
static void
KnownAssignedXidsReset(void)
{
    /* use volatile pointer to prevent code rearrangement */
    volatile ProcArrayStruct *pArray = procArray;

    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    pArray->numKnownAssignedXids = 0;
    pArray->tailKnownAssignedXids = 0;
    pArray->headKnownAssignedXids = 0;

    LWLockRelease(ProcArrayLock);
}

/*
 * Do a consistency check on the running processes. Cluster Monitor uses this
 * API to check if some transaction has started with an xid or xmin lower than
 * the GlobalXmin reported by the GTM. This can only happen under extreme
 * conditions and we must take necessary steps to safe-guard against such
 * anomalies.
 */
void
ProcArrayCheckXminConsistency(TransactionId global_xmin)
{
    volatile ProcArrayStruct *arrayP = procArray;
    int            index;

    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        TransactionId xid;

        xid = pgxact->xid;        /* fetch just once */
        if (!TransactionIdIsNormal(xid))
            continue;

        if (!TransactionIdFollowsOrEquals(xid, global_xmin))
            elog(PANIC, "xmin consistency check failed - found %d xid in "
                    "PGPROC %d ahead of GlobalXmin %d", xid, pgprocno,
                    global_xmin);

        xid = pgxact->xmin;        /* fetch just once */
        if (!TransactionIdIsNormal(xid))
            continue;

        if (!TransactionIdFollowsOrEquals(xid, global_xmin))
            elog(PANIC, "xmin consistency check failed - found %d xmin in "
                    "PGPROC %d ahead of GlobalXmin %d", xid, pgprocno,
                    global_xmin);
    }
}

void
SetLatestCompletedXid(TransactionId latestCompletedXid)
{
    if (!TransactionIdIsValid(latestCompletedXid))
        return;
    /*
     * First extend the commit logs. Even though we may not have actually
     * started any transactions in the new range, we must still extend the logs
     * so that later operations which may try to query them based on the new
     * value of latestCompletedXid do not throw errors
     */
    ExtendLogs(latestCompletedXid);

    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

    if (TransactionIdPrecedes(latestCompletedXid,
                ShmemVariableCache->latestCompletedXid))
    {
        LWLockRelease(ProcArrayLock);
        return;
    }

    ShmemVariableCache->latestCompletedXid = latestCompletedXid;
    LWLockRelease(ProcArrayLock);
}
#ifdef __TBASE__
/* 
  * get current running transactions 
  * do not include lazy vacuum and itself
  */
RunningTransactions
GetCurrentRunningTransaction(void)
{// #lizard forgives
    /* result workspace */
    static RunningTransactionsData CurrentRunningXactsData;

    ProcArrayStruct *arrayP = procArray;
    RunningTransactions CurrentRunningXacts = &CurrentRunningXactsData;
    TransactionId latestCompletedXid;
    TransactionId oldestRunningXid;
    TransactionId *xids;
    int            index;
    int            count;
    int            subcount;
    bool        suboverflowed;

    Assert(!RecoveryInProgress());

    /*
     * Allocating space for maxProcs xids is usually overkill; numProcs would
     * be sufficient.  But it seems better to do the malloc while not holding
     * the lock, so we can't look at numProcs.  Likewise, we allocate much
     * more subxip storage than is probably needed.
     *
     * Should only be allocated in bgwriter, since only ever executed during
     * checkpoints.
     */
    if (CurrentRunningXacts->xids == NULL)
    {
        /*
         * First call
         */
        CurrentRunningXacts->xids = (TransactionId *)
            malloc(TOTAL_MAX_CACHED_SUBXIDS * sizeof(TransactionId));
        if (CurrentRunningXacts->xids == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of memory")));
    }

    xids = CurrentRunningXacts->xids;

    count = subcount = 0;
    suboverflowed = false;

    /*
     * Ensure that no xids enter or leave the procarray while we obtain
     * snapshot.
     */
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    LWLockAcquire(XidGenLock, LW_SHARED);

    latestCompletedXid = ShmemVariableCache->latestCompletedXid;

    oldestRunningXid = ShmemVariableCache->nextXid;

    /*
     * Spin over procArray collecting all xids and subxids.
     */
    for (index = 0; index < arrayP->numProcs; index++)
    {
        int            pgprocno = arrayP->pgprocnos[index];
        volatile PGPROC *proc = &allProcs[pgprocno];
        volatile PGXACT *pgxact = &allPgXact[pgprocno];
        TransactionId xid;
        int            nxids;

        /* Fetch xid just once - see GetNewTransactionId */
        xid = pgxact->xid;

        /*
         * We don't need to store transactions that don't have a TransactionId
         * yet because they will not show as running on a standby server.
         */
        if (!TransactionIdIsValid(xid))
            continue;
        
        /* Ignore procs running LAZY VACUUM */
        if (pgxact->vacuumFlags & PROC_IN_VACUUM)
            continue;    
        
        if (pgxact == MyPgXact)
            continue;


        xids[count++] = xid;

        if (TransactionIdPrecedes(xid, oldestRunningXid))
            oldestRunningXid = xid;

        /*
         * Save subtransaction XIDs. Other backends can't add or remove
         * entries while we're holding XidGenLock.
         */
        nxids = pgxact->nxids;
        if (nxids > 0)
        {
            memcpy(&xids[count], (void *) proc->subxids.xids,
                   nxids * sizeof(TransactionId));
            count += nxids;
            subcount += nxids;

            if (pgxact->overflowed)
                suboverflowed = true;

            /*
             * Top-level XID of a transaction is always less than any of its
             * subxids, so we don't need to check if any of the subxids are
             * smaller than oldestRunningXid
             */
        }
    }

    CurrentRunningXacts->xcnt = count;
    CurrentRunningXacts->subxid_overflow = suboverflowed;
    CurrentRunningXacts->nextXid = ShmemVariableCache->nextXid;
    CurrentRunningXacts->oldestRunningXid = oldestRunningXid;
    CurrentRunningXacts->latestCompletedXid = latestCompletedXid;

    /* We don't release XidGenLock here, the caller is responsible for that */
    LWLockRelease(ProcArrayLock);

    Assert(TransactionIdIsValid(CurrentRunningXacts->nextXid));
    Assert(TransactionIdIsValid(CurrentRunningXacts->oldestRunningXid));
    Assert(TransactionIdIsNormal(CurrentRunningXacts->latestCompletedXid));

    return CurrentRunningXacts;

}

/* Get global gts, here we don't take lock, just ensure the 64 bits assign is atomic. */
GlobalTimestamp GetLatestCommitTS(void)
{
    GlobalTimestamp gts;
    gts = ShmemVariableCache->latestGTS;
    return gts;
}
#endif
