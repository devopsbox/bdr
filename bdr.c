/* -------------------------------------------------------------------------
 *
 * bdr.c
 *		Replication!!!
 *
 * Replication???
 *
 * Copyright (C) 2012-2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		bdr.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "bdr.h"
#include "bdr_locks.h"
#include "bdr_label.h"

#include "libpq-fe.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port.h"

#include "access/committs.h"
#include "access/seqam.h"
#include "access/heapam.h"
#include "access/xact.h"

#include "catalog/namespace.h"
#include "catalog/pg_extension.h"

#include "commands/dbcommands.h"
#include "commands/extension.h"

#include "lib/stringinfo.h"

#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"

#include "nodes/execnodes.h"

#include "postmaster/bgworker.h"

#include "replication/replication_identifier.h"

#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#define MAXCONNINFO		1024

volatile sig_atomic_t got_SIGTERM = false;
volatile sig_atomic_t got_SIGHUP = false;

extern uint64		origin_sysid;
extern TimeLineID	origin_timeline;
extern Oid			origin_dboid;
/* end externs for bdr apply state */

ResourceOwner bdr_saved_resowner;
Oid   BdrSchemaOid = InvalidOid;
Oid   BdrNodesRelid = InvalidOid;
Oid   BdrConnectionsRelid = InvalidOid;
Oid   BdrConflictHistoryRelId = InvalidOid;
Oid   BdrLocksRelid = InvalidOid;
Oid   BdrLocksByOwnerRelid = InvalidOid;
Oid   BdrReplicationSetConfigRelid = InvalidOid;
Oid   BdrSeqamOid = InvalidOid;
Oid   BdrSupervisorDbOid = InvalidOid;

/* GUC storage */
static bool bdr_synchronous_commit;
int bdr_default_apply_delay;
int bdr_max_workers;
int bdr_max_databases;
static bool bdr_skip_ddl_replication;
bool bdr_skip_ddl_locking;
bool bdr_do_not_replicate;
bool bdr_trace_replay;
int bdr_trace_ddl_locks_level;
char *bdr_extra_apply_connection_options;

PG_MODULE_MAGIC;

void		_PG_init(void);

PGDLLEXPORT Datum bdr_apply_pause(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_apply_resume(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_apply_is_paused(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_version(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_version_num(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_min_remote_version_num(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_variant(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_get_local_nodeid(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_parse_slot_name_sql(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_parse_replident_name_sql(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_format_slot_name_sql(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_format_replident_name_sql(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_terminate_walsender_workers_byname(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_terminate_apply_workers_byname(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_terminate_walsender_workers(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_terminate_apply_workers(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_skip_changes_upto(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bdr_pause_worker_management(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(bdr_apply_pause);
PG_FUNCTION_INFO_V1(bdr_apply_resume);
PG_FUNCTION_INFO_V1(bdr_apply_is_paused);
PG_FUNCTION_INFO_V1(bdr_version);
PG_FUNCTION_INFO_V1(bdr_version_num);
PG_FUNCTION_INFO_V1(bdr_min_remote_version_num);
PG_FUNCTION_INFO_V1(bdr_variant);
PG_FUNCTION_INFO_V1(bdr_get_local_nodeid);
PG_FUNCTION_INFO_V1(bdr_parse_slot_name_sql);
PG_FUNCTION_INFO_V1(bdr_parse_replident_name_sql);
PG_FUNCTION_INFO_V1(bdr_format_slot_name_sql);
PG_FUNCTION_INFO_V1(bdr_format_replident_name_sql);
PG_FUNCTION_INFO_V1(bdr_terminate_walsender_workers_byname);
PG_FUNCTION_INFO_V1(bdr_terminate_apply_workers_byname);
PG_FUNCTION_INFO_V1(bdr_terminate_walsender_workers);
PG_FUNCTION_INFO_V1(bdr_terminate_apply_workers);
PG_FUNCTION_INFO_V1(bdr_skip_changes_upto);
PG_FUNCTION_INFO_V1(bdr_pause_worker_management);

static bool bdr_terminate_workers_byid(uint64 sysid, TimeLineID timeline,
	Oid dboid, BdrWorkerType worker_type);

static const struct config_enum_entry bdr_trace_ddl_locks_level_options[] = {
	{"debug", DDL_LOCK_TRACE_DEBUG, false},
	{"peers", DDL_LOCK_TRACE_PEERS, false},
	{"acquire_release", DDL_LOCK_TRACE_ACQUIRE_RELEASE, false},
	{"statement", DDL_LOCK_TRACE_STATEMENT, false},
	{"none", DDL_LOCK_TRACE_NONE, false},
	{NULL, 0, false}
};

void
bdr_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGTERM = true;

	/*
	 * For now allow to interrupt all queries. It'd be better if we were more
	 * granular, only allowing to interrupt some things, but that's a bit
	 * harder than we have time for right now.
	 */
	InterruptPending = true;
	ProcDiePending = true;

	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

void
bdr_sighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGHUP = true;

	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Get database Oid of the remotedb.
 */
static Oid
bdr_get_remote_dboid(const char *conninfo_db)
{
	PGconn	   *dbConn;
	PGresult   *res;
	char	   *remote_dboid;
	Oid			remote_dboid_i;

	elog(DEBUG3, "Fetching database oid via standard connection");

	dbConn = PQconnectdb(conninfo_db);
	if (PQstatus(dbConn) != CONNECTION_OK)
	{
		ereport(FATAL,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("get remote OID: %s", PQerrorMessage(dbConn)),
				 errdetail("Connection string is '%s'", conninfo_db)));
	}

	res = PQexec(dbConn, "SELECT oid FROM pg_database WHERE datname = current_database()");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		elog(FATAL, "could not fetch database oid: %s",
			 PQerrorMessage(dbConn));
	}
	if (PQntuples(res) != 1 || PQnfields(res) != 1)
	{
		elog(FATAL, "could not identify system: got %d rows and %d fields, expected %d rows and %d fields\n",
			 PQntuples(res), PQnfields(res), 1, 1);
	}

	remote_dboid = PQgetvalue(res, 0, 0);
	if (sscanf(remote_dboid, "%u", &remote_dboid_i) != 1)
		elog(ERROR, "could not parse remote database OID %s", remote_dboid);

	PQclear(res);
	PQfinish(dbConn);

	return remote_dboid_i;
}

/*
 * Format a slot name and replication identifier for a connection to a remote
 * node, suitable for use as the slot name on the remote server and as the
 * local replication identifier for that slot. Uses the local node's current
 * dboid.
 *
 * Does NOT enforce that the remote and local node identities must differ.
 *
 * The replication identifier is allocated in the current memory context.
 */
static void
bdr_build_ident_and_slotname(uint64 remote_sysid, TimeLineID remote_tlid,
		Oid remote_dboid, char **out_replication_identifier,
		Name out_slot_name)
{
	Assert(MyDatabaseId != InvalidOid);
	Assert(remote_dboid != InvalidOid);

	bdr_slot_name(out_slot_name, GetSystemIdentifier(), ThisTimeLineID, MyDatabaseId,
				  remote_dboid);

	*out_replication_identifier = bdr_replident_name(remote_sysid, remote_tlid,
			remote_dboid, MyDatabaseId);
}

/*
 * Establish a BDR connection
 *
 * Connects to the remote node, identifies it, and generates local and remote
 * replication identifiers and slot name. The conninfo string passed should
 * specify a dbname. It must not contain a replication= parameter.
 *
 * Does NOT enforce that the remote and local node identities must differ.
 *
 * appname may be NULL.
 *
 * The local replication identifier is not saved, the caller must do that.
 *
 * Returns the PGconn for the established connection.
 *
 * Sets out parameters:
 *   remote_ident
 *   slot_name
 *   remote_sysid_i
 *   remote_tlid_i
 */
PGconn*
bdr_connect(const char *conninfo,
			Name appname,
			uint64* remote_sysid_i, TimeLineID *remote_tlid_i,
			Oid *remote_dboid_i)
{
	PGconn	   *streamConn;
	PGresult   *res;
	StringInfoData conninfo_repl;
	char	   *remote_sysid;
	char	   *remote_tlid;
	char		local_sysid[32];

	initStringInfo(&conninfo_repl);

	appendStringInfo(&conninfo_repl, "replication=database "
									 "fallback_application_name='%s' ",
			(appname == NULL ? "bdr" : NameStr(*appname)));

	appendStringInfoString(&conninfo_repl, bdr_default_apply_connection_options);
	appendStringInfoChar(&conninfo_repl, ' ');
	appendStringInfoString(&conninfo_repl, bdr_extra_apply_connection_options);
	appendStringInfoChar(&conninfo_repl, ' ');
	appendStringInfoString(&conninfo_repl, conninfo);

	streamConn = PQconnectdb(conninfo_repl.data);
	if (PQstatus(streamConn) != CONNECTION_OK)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("establish BDR: %s", PQerrorMessage(streamConn)),
				 errdetail("Connection string is '%s'", conninfo_repl.data)));
	}

	elog(DEBUG3, "Sending replication command: IDENTIFY_SYSTEM");

	res = PQexec(streamConn, "IDENTIFY_SYSTEM");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		elog(ERROR, "could not send replication command \"%s\": %s",
			 "IDENTIFY_SYSTEM", PQerrorMessage(streamConn));
	}
	if (PQntuples(res) != 1 || PQnfields(res) < 4 || PQnfields(res) > 5)
	{
		elog(ERROR, "could not identify system: got %d rows and %d fields, expected %d rows and %d or %d fields\n",
			 PQntuples(res), PQnfields(res), 1, 4, 5);
	}

	remote_sysid = PQgetvalue(res, 0, 0);
	remote_tlid = PQgetvalue(res, 0, 1);
	if (PQnfields(res) == 5)
	{
		char	   *remote_dboid = PQgetvalue(res, 0, 4);

		if (sscanf(remote_dboid, "%u", remote_dboid_i) != 1)
			elog(ERROR, "could not parse remote database OID %s", remote_dboid);
	}
	else
	{
		*remote_dboid_i = bdr_get_remote_dboid(conninfo);
	}

	if (sscanf(remote_sysid, UINT64_FORMAT, remote_sysid_i) != 1)
		elog(ERROR, "could not parse remote sysid %s", remote_sysid);

	if (sscanf(remote_tlid, "%u", remote_tlid_i) != 1)
		elog(ERROR, "could not parse remote tlid %s", remote_tlid);

	snprintf(local_sysid, sizeof(local_sysid), UINT64_FORMAT,
			 GetSystemIdentifier());

	elog(DEBUG2, "local node (%s,%u,%u), remote node (%s,%s,%u)",
		 local_sysid, ThisTimeLineID, MyDatabaseId, remote_sysid,
		 remote_tlid, *remote_dboid_i);

	/* no parts of IDENTIFY_SYSTEM's response needed anymore */
	PQclear(res);

	return streamConn;
}

/*
 * ----------
 * Create a slot on a remote node, and the corresponding local replication
 * identifier.
 *
 * Arguments:
 *   streamConn		Connection to use for slot creation
 *   slot_name		Name of the slot to create
 *   remote_ident	Identifier for the remote end
 *
 * Out parameters:
 *   replication_identifier		Created local replication identifier
 *   snapshot					If !NULL, snapshot ID of slot snapshot
 *
 * If a snapshot is returned it must be pfree()'d by the caller.
 * ----------
 */
/*
 * TODO we should really handle the case where the slot already exists but
 * there's no local replication identifier, by dropping and recreating the
 * slot.
 */
static void
bdr_create_slot(PGconn *streamConn, Name slot_name,
				char *remote_ident, RepNodeId *replication_identifier,
				char **snapshot)
{
	StringInfoData query;
	PGresult   *res;

	initStringInfo(&query);

	StartTransactionCommand();

	/* we want the new identifier on stable storage immediately */
	ForceSyncCommit();

	/* acquire remote decoding slot */
	resetStringInfo(&query);
	appendStringInfo(&query, "CREATE_REPLICATION_SLOT \"%s\" LOGICAL %s",
					 NameStr(*slot_name), "bdr");

	elog(DEBUG3, "Sending replication command: %s", query.data);

	res = PQexec(streamConn, query.data);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		/* TODO: Should test whether this error is 'already exists' and carry on */

		elog(FATAL, "could not send replication command \"%s\": status %s: %s\n",
			 query.data,
			 PQresStatus(PQresultStatus(res)), PQresultErrorMessage(res));
	}

	/* acquire new local identifier, but don't commit */
	*replication_identifier = CreateReplicationIdentifier(remote_ident);

	/* now commit local identifier */
	CommitTransactionCommand();
	CurrentResourceOwner = bdr_saved_resowner;
	elog(DEBUG1, "created replication identifier %u", *replication_identifier);

	if (snapshot)
		*snapshot = pstrdup(PQgetvalue(res, 0, 2));

	PQclear(res);
}

/*
 * Perform setup work common to all bdr worker types, such as:
 *
 * - set signal handers and unblock signals
 * - Establish db connection
 * - set search_path
 *
 */
void
bdr_bgworker_init(uint32 worker_arg, BdrWorkerType worker_type)
{
	uint16	worker_generation;
	uint16	worker_idx;
	char   *dbname;

	Assert(IsBackgroundWorker);

	worker_generation = (uint16)(worker_arg >> 16);
	worker_idx = (uint16)(worker_arg & 0x0000FFFF);

	if (worker_generation != BdrWorkerCtl->worker_generation)
	{
		elog(DEBUG1, "BDR apply or perdb worker from generation %d exiting after finding shmem generation is %d",
			 worker_generation, BdrWorkerCtl->worker_generation);
		proc_exit(0);
	}

	bdr_worker_shmem_acquire(worker_type, worker_idx, false);

	/* figure out database to connect to */
	if (worker_type == BDR_WORKER_PERDB)
		dbname = NameStr(bdr_worker_slot->data.perdb.dbname);
	else if (worker_type == BDR_WORKER_APPLY)
	{
		BdrApplyWorker	*apply;
		BdrPerdbWorker	*perdb;

		apply = &bdr_worker_slot->data.apply;
		Assert(apply->perdb != NULL);
		perdb = &apply->perdb->data.perdb;

		dbname = NameStr(perdb->dbname);
	}
	else
		elog(FATAL, "don't know how to connect to this type of work: %u",
			 bdr_worker_type);

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, bdr_sighup);
	pqsignal(SIGTERM, bdr_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection(dbname, NULL);

	LWLockAcquire(BdrWorkerCtl->lock, LW_EXCLUSIVE);
	bdr_worker_slot->worker_pid = MyProcPid;
	bdr_worker_slot->worker_proc = MyProc;
	LWLockRelease(BdrWorkerCtl->lock);

	/* make sure BDR extension is up2date */
	bdr_executor_always_allow_writes(true);
	StartTransactionCommand();
	bdr_maintain_schema(true);
	CommitTransactionCommand();
	bdr_executor_always_allow_writes(false);

	/* always work in our own schema */
	SetConfigOption("search_path", "bdr, pg_catalog",
					PGC_BACKEND, PGC_S_OVERRIDE);

	/* setup synchronous commit according to the user's wishes */
	SetConfigOption("synchronous_commit",
					bdr_synchronous_commit ? "local" : "off",
					PGC_BACKEND, PGC_S_OVERRIDE);	/* other context? */

	if (worker_type == BDR_WORKER_APPLY)
	{
		/* Run as replica session replication role, this avoids FK checks. */
		SetConfigOption("session_replication_role", "replica",
						PGC_SUSET, PGC_S_OVERRIDE);	/* other context? */
	}

	/*
	 * Disable function body checks during replay. That's necessary because a)
	 * the creator of the function might have had it disabled b) the function
	 * might be search_path dependant and we don't fix the contents of
	 * functions.
	 */
	SetConfigOption("check_function_bodies", "off",
					PGC_INTERNAL, PGC_S_OVERRIDE);

}

/*
 * Re-usable common error message
 */
void
bdr_error_nodeids_must_differ(uint64 sysid, TimeLineID timeline, Oid dboid)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_NAME),
			 errmsg("The system identifier, timeline ID and/or database oid must differ between the nodes"),
			 errdetail("Both keys are (sysid, timelineid, dboid) = ("UINT64_FORMAT",%u,%u)",
				 sysid, timeline, dboid)));
}

/*
 *----------------------
 * Connect to the BDR remote end, IDENTIFY_SYSTEM, and CREATE_SLOT if necessary.
 * Generates slot name, replication identifier.
 *
 * Raises an error on failure, will not return null.
 *
 * Arguments:
 *	  connection_name:  bdr conn name from bdr.connections to get dsn from
 *
 * Returns:
 *    the libpq connection
 *
 * Out parameters:
 *    out_slot_name: the generated name of the slot on the remote end
 *    out_sysid:     the remote end's system identifier
 *    out_timeline:  the remote end's current timeline
 *    out_replication_identifier: The replication identifier for this connection
 *
 *----------------------
 */
PGconn*
bdr_establish_connection_and_slot(const char *dsn,
	const char *application_name_suffix, Name out_slot_name, uint64 *out_sysid,
	TimeLineID* out_timeline, Oid *out_dboid,
	RepNodeId *out_replication_identifier, char **out_snapshot)
{
	PGconn		*streamConn;
	bool		tx_started = false;
	NameData	appname;
	char		*remote_ident;

	/*
	 * Make sure the local and remote nodes aren't the same node.
	 */
	if (GetSystemIdentifier() == *out_sysid
		&& ThisTimeLineID == *out_timeline
		&& MyDatabaseId == *out_dboid)
	{
		bdr_error_nodeids_must_differ(*out_sysid, *out_timeline, *out_dboid);
	}

	snprintf(NameStr(appname), NAMEDATALEN, BDR_LOCALID_FORMAT":%s",
			BDR_LOCALID_FORMAT_ARGS, application_name_suffix);

	/*
	 * Establish BDR conn and IDENTIFY_SYSTEM, ERROR on things like
	 * connection failure.
	 */
	streamConn = bdr_connect(
		dsn, &appname, out_sysid, out_timeline, out_dboid);

	bdr_build_ident_and_slotname(*out_sysid, *out_timeline, *out_dboid,
			&remote_ident, out_slot_name);

	Assert(remote_ident != NULL);

	if (!IsTransactionState())
	{
		tx_started = true;
		StartTransactionCommand();
	}
	*out_replication_identifier = GetReplicationIdentifier(remote_ident, true);
	if (tx_started)
		CommitTransactionCommand();

	if (OidIsValid(*out_replication_identifier))
	{
		elog(DEBUG1, "found valid replication identifier %u",
			 *out_replication_identifier);
		if (out_snapshot)
			*out_snapshot = NULL;
	}
	else
	{
		/*
		 * Slot doesn't exist, create it.
		 *
		 * The per-db worker will create slots when we first init BDR, but new workers
		 * added afterwards are expected to create their own slots at connect time; that's
		 * when this runs.
		 */

		/* create local replication identifier and a remote slot */
		elog(DEBUG1, "Creating new slot %s", NameStr(*out_slot_name));
		bdr_create_slot(streamConn, out_slot_name, remote_ident,
						out_replication_identifier, out_snapshot);
	}

	pfree(remote_ident);
	remote_ident = NULL;

	return streamConn;
}

static bool
bdr_do_not_replicate_check_hook(bool *newvalue, void **extra, GucSource source)
{
	if (!(*newvalue))
		/* False is always acceptable */
		return true;

	/*
	 * Only set bdr.do_not_replicate if configured via startup packet from the
	 * client application. This prevents possibly unsafe accesses to the
	 * replication identifier state in postmaster context, etc.
	 */
	if (source != PGC_S_CLIENT)
		return false;

	Assert(IsUnderPostmaster);
	Assert(!IsBackgroundWorker);

	return true;
}

/*
 * Override the origin replication identifier that this session will record for
 * its transactions. We need this mainly when applying dumps during
 * init_replica.
 */
static void
bdr_do_not_replicate_assign_hook(bool newvalue, void *extra)
{
	/* Mark these transactions as not to be replicated to other nodes */
	if (newvalue)
		replication_origin_id = DoNotReplicateRepNodeId;
	else
		replication_origin_id = InvalidRepNodeId;
}


/*
 * Entrypoint of this module - called at shared_preload_libraries time in the
 * context of the postmaster.
 *
 * Can't use SPI, and should do as little as sensibly possible. Must initialize
 * any PGC_POSTMASTER custom GUCs, register static bgworkers, as that can't be
 * done later.
 */
void
_PG_init(void)
{
	MemoryContext old_context;

	if (!IsBinaryUpgrade)
	{
		if (!process_shared_preload_libraries_in_progress)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("bdr can only be loaded via shared_preload_libraries")));

		if (!commit_ts_enabled)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("bdr requires \"track_commit_timestamp\" to be enabled")));
	}

	/*
	 * Force btree_gist to be loaded - its absolutely not required at this
	 * point, but since it's required for BDR to be used it's much easier to
	 * debug if we error out during start than failing during background
	 * worker initialization.
	 */
	load_external_function("btree_gist", "gbtreekey_in", true, NULL);

	/* guc's et al need to survive outside the lifetime of the library init */
	old_context = MemoryContextSwitchTo(TopMemoryContext);

	/* XXX: make it changeable at SIGHUP? */
	DefineCustomBoolVariable("bdr.synchronous_commit",
							 "bdr specific synchronous commit value",
							 NULL,
							 &bdr_synchronous_commit,
							 false, PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("bdr.log_conflicts_to_table",
							 "Log BDR conflicts to bdr.conflict_history table",
							 NULL,
							 &bdr_log_conflicts_to_table,
							 false,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("bdr.conflict_logging_include_tuples",
							 "Log whole tuples when logging BDR conflicts",
							 NULL,
							 &bdr_conflict_logging_include_tuples,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("bdr.permit_ddl_locking",
							 "Allow commands that can acquire the global "
							 "DDL lock",
							 NULL,
							 &bdr_permit_ddl_locking,
							 true, PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("bdr.permit_unsafe_ddl_commands",
							 "Allow commands that might cause data or " \
							 "replication problems under BDR to run",
							 NULL,
							 &bdr_permit_unsafe_commands,
							 false, PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("bdr.skip_ddl_replication",
							 "Internal. Set during local restore during init_replica only",
							 NULL,
							 &bdr_skip_ddl_replication,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("bdr.skip_ddl_locking",
							 "Don't acquire global DDL locks while performing DDL.",
							 "Note that it's quite dangerous to do so.",
							 &bdr_skip_ddl_locking,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("bdr.default_apply_delay",
							"default replication apply delay, can be overwritten per connection",
							NULL,
							&bdr_default_apply_delay,
							0, 0, INT_MAX,
							PGC_SIGHUP,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	DefineCustomIntVariable("bdr.max_ddl_lock_delay",
							"Sets the maximum delay before canceling queries while waiting for global lock",
							"If se to -1 max_standby_streaming_delay will be used",
							&bdr_max_ddl_lock_delay,
							-1, -1, INT_MAX,
							PGC_SIGHUP,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	DefineCustomIntVariable("bdr.bdr_ddl_lock_timeout",
							"Sets the maximum allowed duration of any wait for a global lock",
							"If se to -1 lock_timeout will be used",
							&bdr_ddl_lock_timeout,
							-1, -1, INT_MAX,
							PGC_SIGHUP,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	/*
	 * We can't use the temp_tablespace safely for our dumps, because Pg's
	 * crash recovery is very careful to delete only particularly formatted
	 * files. Instead for now just allow user to specify dump storage.
	 */
	DefineCustomStringVariable("bdr.temp_dump_directory",
							   "Directory to store dumps for local restore",
							   NULL,
							   &bdr_temp_dump_directory,
							   "/tmp", PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("bdr.do_not_replicate",
							 "Internal. Set during local initialization from basebackup only",
							 NULL,
							 &bdr_do_not_replicate,
							 false,
							 PGC_BACKEND,
							 0,
							 bdr_do_not_replicate_check_hook,
							 bdr_do_not_replicate_assign_hook,
							 NULL);

	DefineCustomBoolVariable("bdr.trace_replay",
							 "Log each remote action as it is received",
							 NULL,
							 &bdr_trace_replay,
							 false, PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("bdr.trace_ddl_locks_level",
							 "Log DDL locking activity at this log level",
							 NULL,
							 &bdr_trace_ddl_locks_level,
							 DDL_LOCK_TRACE_STATEMENT,
							 bdr_trace_ddl_locks_level_options,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("bdr.extra_apply_connection_options",
							   "connection options to add to all peer node connections",
							   NULL,
							   &bdr_extra_apply_connection_options,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	EmitWarningsOnPlaceholders("bdr");

	bdr_label_init();

	if (!IsBinaryUpgrade)
	{

		bdr_supervisor_register();

		/*
		 * Reserve shared memory segment to store bgworker connection information
		 * and hook into shmem initialization.
		 */
		bdr_shmem_init();

		bdr_executor_init();

		/* Set up a ProcessUtility_hook to stop unsupported commands being run */
		init_bdr_commandfilter();
	}

	MemoryContextSwitchTo(old_context);
}

Oid
bdr_lookup_relid(const char *relname, Oid schema_oid)
{
	Oid			relid;

	relid = get_relname_relid(relname, schema_oid);

	if (!relid)
		elog(ERROR, "cache lookup failed for relation %s.%s",
			 get_namespace_name(schema_oid), relname);

	return relid;
}

/*
 * Make sure all required extensions are installed in the correct version for
 * the current database.
 *
 * Concurrent executions will block, but not fail.
 *
 * Must be called inside transaction.
 *
 * If update_extensions is true, ALTER EXTENSION commands will be issued to
 * ensure the required extension(s) are at the current version.
 */
void
bdr_maintain_schema(bool update_extensions)
{
	Relation	extrel;
	Oid			btree_gist_oid;
	Oid			bdr_oid;
	Oid			schema_oid;

	PushActiveSnapshot(GetTransactionSnapshot());

	set_config_option("bdr.skip_ddl_replication", "true",
					  PGC_SUSET, PGC_S_OVERRIDE, GUC_ACTION_LOCAL,
					  true, 0
#if PG_VERSION_NUM >= 90500
							 , false
#endif
							);

	/* make sure we're operating without other bdr workers interfering */
	extrel = heap_open(ExtensionRelationId, ShareUpdateExclusiveLock);

	btree_gist_oid = get_extension_oid("btree_gist", true);
	bdr_oid = get_extension_oid("bdr", true);

	if (btree_gist_oid == InvalidOid)
		elog(ERROR, "btree_gist is required by BDR but not installed in the current database");

	if (bdr_oid == InvalidOid)
		elog(ERROR, "bdr extension is not installed in the current database");

	if (update_extensions)
	{
		AlterExtensionStmt alter_stmt;

		/* TODO: only do this if necessary */
		alter_stmt.options = NIL;
		alter_stmt.extname = (char *)"btree_gist";
		ExecAlterExtensionStmt(&alter_stmt);

		/* TODO: only do this if necessary */
		alter_stmt.options = NIL;
		alter_stmt.extname = (char *)"bdr";
		ExecAlterExtensionStmt(&alter_stmt);
	}

	heap_close(extrel, NoLock);

	/* setup initial queued_cmds OID */
	schema_oid = get_namespace_oid("bdr", false);
	BdrSchemaOid = schema_oid;
	BdrNodesRelid =
		bdr_lookup_relid("bdr_nodes", schema_oid);
	BdrConnectionsRelid =
		bdr_lookup_relid("bdr_connections", schema_oid);
	QueuedDDLCommandsRelid =
		bdr_lookup_relid("bdr_queued_commands", schema_oid);
	BdrConflictHistoryRelId =
		bdr_lookup_relid("bdr_conflict_history", schema_oid);
	BdrReplicationSetConfigRelid  =
		bdr_lookup_relid("bdr_replication_set_config", schema_oid);
	BdrSequenceValuesRelid =
		bdr_lookup_relid("bdr_sequence_values", schema_oid);
	BdrSequenceElectionsRelid =
		bdr_lookup_relid("bdr_sequence_elections", schema_oid);
	BdrVotesRelid =
		bdr_lookup_relid("bdr_votes", schema_oid);
	QueuedDropsRelid =
		bdr_lookup_relid("bdr_queued_drops", schema_oid);
	BdrLocksRelid =
		bdr_lookup_relid("bdr_global_locks", schema_oid);
	BdrLocksByOwnerRelid =
		bdr_lookup_relid("bdr_global_locks_byowner", schema_oid);
	BdrSeqamOid = get_seqam_oid("bdr", false);
	BdrSupervisorDbOid = bdr_get_supervisordb_oid(false);

	bdr_conflict_handlers_init();

	PopActiveSnapshot();
}

Datum
bdr_apply_pause(PG_FUNCTION_ARGS)
{
	/*
	 * It's safe to pause without grabbing the segment lock;
	 * an overlapping resume won't do any harm.
	 */
	BdrWorkerCtl->pause_apply = true;
	PG_RETURN_VOID();
}

Datum
bdr_apply_resume(PG_FUNCTION_ARGS)
{
	int i;

	LWLockAcquire(BdrWorkerCtl->lock, LW_SHARED);
	BdrWorkerCtl->pause_apply = false;

	/*
	 * To get apply workers to notice immediately we have to set all their
	 * latches. This will also force config reloads, but that's cheap and
	 * harmless.
	 */
	for (i = 0; i < bdr_max_workers; i++)
	{
		BdrWorker *w = &BdrWorkerCtl->slots[i];
		if (w->worker_type == BDR_WORKER_APPLY)
		{
			BdrApplyWorker *apply = &w->data.apply;
			SetLatch(apply->proclatch);
		}
	}

	LWLockRelease(BdrWorkerCtl->lock);
	PG_RETURN_VOID();
}

Datum
bdr_apply_is_paused(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(BdrWorkerCtl->pause_apply);
}

Datum
bdr_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(BDR_VERSION_STR));
}

Datum
bdr_version_num(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(BDR_VERSION_NUM);
}

Datum
bdr_min_remote_version_num(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(BDR_MIN_REMOTE_VERSION_NUM);
}

Datum
bdr_variant(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(BDR_VARIANT));
}

/* Return a tuple of (sysid oid, tlid oid, dboid oid) */
Datum
bdr_get_local_nodeid(PG_FUNCTION_ARGS)
{
	Datum		values[3];
	bool		isnull[3] = {false, false, false};
	TupleDesc	tupleDesc;
	HeapTuple	returnTuple;
	char		sysid_str[33];

	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	snprintf(sysid_str, sizeof(sysid_str), UINT64_FORMAT, GetSystemIdentifier());
	sysid_str[sizeof(sysid_str)-1] = '\0';

	values[0] = CStringGetTextDatum(sysid_str);
	values[1] = ObjectIdGetDatum(ThisTimeLineID);
	values[2] = ObjectIdGetDatum(MyDatabaseId);

	returnTuple = heap_form_tuple(tupleDesc, values, isnull);

	PG_RETURN_DATUM(HeapTupleGetDatum(returnTuple));
}

Datum
bdr_parse_slot_name_sql(PG_FUNCTION_ARGS)
{
	const char 	*slot_name = NameStr(*PG_GETARG_NAME(0));
	Datum		values[5];
	bool		isnull[5] = {false, false, false, false, false};
	TupleDesc	tupleDesc;
	HeapTuple	returnTuple;
	char		remote_sysid_str[33];
	uint64		remote_sysid;
	TimeLineID	remote_tli;
	Oid			remote_dboid;
	Oid			local_dboid;

	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	bdr_parse_slot_name(slot_name, &remote_sysid, &remote_tli,
			&remote_dboid, &local_dboid);

	snprintf(remote_sysid_str, sizeof(remote_sysid_str),
			UINT64_FORMAT, remote_sysid);
	remote_sysid_str[sizeof(remote_sysid_str)-1] = '\0';

	values[0] = CStringGetTextDatum(remote_sysid_str);
	values[1] = ObjectIdGetDatum(remote_tli);
	values[2] = ObjectIdGetDatum(remote_dboid);
	values[3] = ObjectIdGetDatum(local_dboid);
	values[4] = CStringGetTextDatum(EMPTY_REPLICATION_NAME);

	returnTuple = heap_form_tuple(tupleDesc, values, isnull);

	PG_RETURN_DATUM(HeapTupleGetDatum(returnTuple));
}

Datum
bdr_parse_replident_name_sql(PG_FUNCTION_ARGS)
{
	const char 	*replident_name = text_to_cstring(PG_GETARG_TEXT_P(0));
	Datum		values[5];
	bool		isnull[5] = {false, false, false, false, false};
	TupleDesc	tupleDesc;
	HeapTuple	returnTuple;
	char		remote_sysid_str[33];
	uint64		remote_sysid;
	TimeLineID	remote_tli;
	Oid			remote_dboid;
	Oid			local_dboid;

	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	bdr_parse_replident_name(replident_name, &remote_sysid, &remote_tli,
			&remote_dboid, &local_dboid);

	snprintf(remote_sysid_str, sizeof(remote_sysid_str),
			UINT64_FORMAT, remote_sysid);
	remote_sysid_str[sizeof(remote_sysid_str)-1] = '\0';

	values[0] = CStringGetTextDatum(remote_sysid_str);
	values[1] = ObjectIdGetDatum(remote_tli);
	values[2] = ObjectIdGetDatum(remote_dboid);
	values[3] = ObjectIdGetDatum(local_dboid);
	values[4] = CStringGetTextDatum(EMPTY_REPLICATION_NAME);

	returnTuple = heap_form_tuple(tupleDesc, values, isnull);

	PG_RETURN_DATUM(HeapTupleGetDatum(returnTuple));
}

Datum
bdr_format_slot_name_sql(PG_FUNCTION_ARGS)
{
	const char	*remote_sysid_str = text_to_cstring(PG_GETARG_TEXT_P(0));
	Oid			remote_tli = PG_GETARG_OID(1);
	Oid			remote_dboid = PG_GETARG_OID(2);
	Oid			local_dboid = PG_GETARG_OID(3);
	const char	*replication_name = NameStr(*PG_GETARG_NAME(4));
	uint64		remote_sysid;
	Name		slot_name;

	if (strlen(replication_name) != 0)
		elog(ERROR, "Non-empty replication_name is not yet supported");

	if (sscanf(remote_sysid_str, UINT64_FORMAT, &remote_sysid) != 1)
		elog(ERROR, "Parsing of remote sysid as uint64 failed");

	slot_name = (Name)palloc0(NAMEDATALEN);

	bdr_slot_name(slot_name, remote_sysid, remote_tli,
			remote_dboid, local_dboid);

	PG_RETURN_NAME(slot_name);
}

Datum
bdr_format_replident_name_sql(PG_FUNCTION_ARGS)
{
	const char	*remote_sysid_str = text_to_cstring(PG_GETARG_TEXT_P(0));
	Oid			remote_tli = PG_GETARG_OID(1);
	Oid			remote_dboid = PG_GETARG_OID(2);
	Oid			local_dboid = PG_GETARG_OID(3);
	const char	*replication_name = NameStr(*PG_GETARG_NAME(4));
	uint64		remote_sysid;
	char*		replident_name;

	if (strlen(replication_name) != 0)
		elog(ERROR, "Non-empty replication_name is not yet supported");

	if (sscanf(remote_sysid_str, UINT64_FORMAT, &remote_sysid) != 1)
		elog(ERROR, "Parsing of remote sysid as uint64 failed");

	replident_name = bdr_replident_name(remote_sysid, remote_tli,
			remote_dboid, local_dboid);

	PG_RETURN_TEXT_P(cstring_to_text(replident_name));
}


/*
 * You should prefer to use bdr_version_num but if you can't
 * then this will be handy.
 *
 * ERRORs if the major/minor/rev can't be parsed.
 *
 * If subrev is absent or cannot be parsed returns -1 for subrev.
 *
 * The return value is the bdr version in BDR_VERSION_NUM form.
 */
int
bdr_parse_version(const char * bdr_version_str,
		int *o_major, int *o_minor, int *o_rev, int *o_subrev)
{
	int nparsed, major, minor, rev, subrev;

	nparsed = sscanf(bdr_version_str, "%d.%d.%d.%d", &major, &minor, &rev, &subrev);

	if (nparsed < 3)
		elog(ERROR, "Unable to parse '%s' as a BDR version number", bdr_version_str);
	else if (nparsed < 4)
		subrev = -1;

	if (o_major != NULL)
		*o_major = major;
	if (o_minor != NULL)
		*o_minor = minor;
	if (o_rev != NULL)
		*o_rev = rev;
	if (o_subrev != NULL)
		*o_subrev = subrev;

	return major * 10000 + minor * 100 + rev;
}

Datum
bdr_skip_changes_upto(PG_FUNCTION_ARGS)
{
	const char	*remote_sysid_str = text_to_cstring(PG_GETARG_TEXT_P(0));
	Oid			remote_tli = PG_GETARG_OID(1);
	Oid			remote_dboid = PG_GETARG_OID(2);
	XLogRecPtr	upto_lsn = PG_GETARG_LSN(3);
	uint64		remote_sysid;
	RepNodeId   nodeid;

	if (!bdr_permit_unsafe_commands)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("skipping changes is unsafe and will cause replicas to be out of sync"),
				 errhint("Set bdr.permit_unsafe_ddl_commands if you are sure you want to do this")));

	if (upto_lsn == InvalidXLogRecPtr)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("Target LSN must be nonzero")));

	if (sscanf(remote_sysid_str, UINT64_FORMAT, &remote_sysid) != 1)
		elog(ERROR, "Parsing of remote sysid as uint64 failed");

	if (remote_sysid == GetSystemIdentifier()
		&& remote_tli == ThisTimeLineID
		&& remote_dboid == MyDatabaseId)
		elog(ERROR, "the passed ID is for the local node, can't skip changes from self");

	/* Only ever matches a replnode id owned by the local BDR node */
	nodeid = bdr_fetch_node_id_via_sysid(remote_sysid,
			(TimeLineID)remote_tli, remote_dboid);

	if (nodeid == InvalidRepNodeId)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no replication identifier found for node")));

	Assert(nodeid != DoNotReplicateRepNodeId);

	AdvanceReplicationIdentifier(nodeid, upto_lsn, XactLastCommitEnd);

	/*
	 * The peer won't notice the replication identifier advance, we need to
	 * tell it to re-check its configuration. While we do support re-reading
	 * configuration via bdr.bdr_connections_changed() that only cares about
	 * changes to bdr_connections, and this is a replication identifier update.
	 * Since we also want the change to take effect promptly, just kill the
	 * relevant apply worker.
	 */
	if (!bdr_terminate_workers_byid(remote_sysid, remote_tli, remote_dboid, BDR_WORKER_APPLY))
	{
		ereport(WARNING,
				(errmsg("advanced replay position but couldn't signal apply worker"),
				 errhint("check if the apply worker for the target node is running and terminate it manually")));
	}

	PG_RETURN_VOID();
}

/*
 * Terminate the worker with the identified role and remote peer that
 * is operating on the current database.
 */
static bool
bdr_terminate_workers_byid(uint64 sysid, TimeLineID timeline, Oid dboid, BdrWorkerType worker_type)
{
	int			pid = 0;
	BdrWorker * worker;

	/*
	 * Right now there can only be one worker for any given remote, so we don't really have
	 * to deal with multiple workers at all.
	 */
	LWLockAcquire(BdrWorkerCtl->lock, LW_SHARED);
	worker = bdr_worker_get_entry(sysid, timeline, dboid, worker_type);

	if (worker != NULL && worker->worker_proc != NULL)
		pid = worker->worker_proc->pid;

	LWLockRelease(BdrWorkerCtl->lock);

	if (pid == 0)
		return false;

	/*
	 * We could call kill() directly but this way we do the permissions checks,
	 * get pgroup handling, etc. It means we look the pid up in PGPROC again,
	 * but that's harmless enough. There's an unavoidable race with pid
	 * recycling no matter what we do and it's no worse whether or not we go
	 * via pg_terminate_backend.
	 */
	return DatumGetBool(DirectFunctionCall1(pg_terminate_backend, Int32GetDatum(pid)));
}

Datum
bdr_terminate_apply_workers(PG_FUNCTION_ARGS)
{
	const char *sysid_str	= text_to_cstring(PG_GETARG_TEXT_P(0));
	uint64		sysid;
	TimeLineID	timeline	= PG_GETARG_OID(1);
	Oid			dboid		= PG_GETARG_OID(2);

	if (sscanf(sysid_str, UINT64_FORMAT, &sysid) != 1)
		elog(ERROR, "couldn't parse sysid as uint64");

	PG_RETURN_BOOL(bdr_terminate_workers_byid(sysid, timeline, dboid, BDR_WORKER_APPLY));
}

Datum
bdr_terminate_walsender_workers(PG_FUNCTION_ARGS)
{
	const char *sysid_str	= text_to_cstring(PG_GETARG_TEXT_P(0));
	uint64		sysid;
	TimeLineID	timeline	= PG_GETARG_OID(1);
	Oid			dboid		= PG_GETARG_OID(2);

	if (sscanf(sysid_str, UINT64_FORMAT, &sysid) != 1)
		elog(ERROR, "couldn't parse sysid as uint64");

	PG_RETURN_BOOL(bdr_terminate_workers_byid(sysid, timeline, dboid, BDR_WORKER_WALSENDER));
}

Datum
bdr_terminate_apply_workers_byname(PG_FUNCTION_ARGS)
{
	const char *node_name = text_to_cstring(PG_GETARG_TEXT_P(0));
	TimeLineID	timeline;
	Oid			dboid;
	uint64		sysid;

	if (!bdr_get_node_identity_by_name(node_name, &sysid, &timeline, &dboid))
		ereport(ERROR,
				(errmsg("named node not found in bdr.bdr_nodes")));

	PG_RETURN_BOOL(bdr_terminate_workers_byid(sysid, timeline, dboid, BDR_WORKER_APPLY));

}

Datum
bdr_terminate_walsender_workers_byname(PG_FUNCTION_ARGS)
{
	const char *node_name = text_to_cstring(PG_GETARG_TEXT_P(0));
	TimeLineID	timeline;
	Oid			dboid;
	uint64		sysid;

	if (!bdr_get_node_identity_by_name(node_name, &sysid, &timeline, &dboid))
		ereport(ERROR,
				(errmsg("named node not found in bdr.bdr_nodes")));

	PG_RETURN_BOOL(bdr_terminate_workers_byid(sysid, timeline, dboid, BDR_WORKER_WALSENDER));
}

/*
 * This function is used for debugging and tests, mainly to make unit tests more
 * predictable. It pauses BDR worker management and stops new worker launches
 * until unpaused.
 *
 * The pause applies across all BDR nodes on the current instance. When unpaused,
 * the caller should signal bdr_connections_changed() on every node.
 *
 * This function is intentionally undocumented and isn't for normal use.
 */
Datum
bdr_pause_worker_management(PG_FUNCTION_ARGS)
{
	bool pause = PG_GETARG_BOOL(0);

	if (pause && !bdr_permit_unsafe_commands)
		elog(ERROR, "this function is for internal test use only");

	LWLockAcquire(BdrWorkerCtl->lock, LW_EXCLUSIVE);
	BdrWorkerCtl->worker_management_paused = pause;
	LWLockRelease(BdrWorkerCtl->lock);

	elog(LOG, "BDR worker management %s", pause ? "paused" : "unpaused");

	PG_RETURN_VOID();
}

/*
 * Report whether BDR is active on the DB.
 */
Datum
bdr_is_active_in_db(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(bdr_is_bdr_activated_db(MyDatabaseId));
}
