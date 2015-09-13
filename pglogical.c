/*-------------------------------------------------------------------------
 *
 * pg_logical_manager.c
 * 		pg_logical worker for managing apply workers in a database
 *
 * Copyright (c) 2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  pg_logical_manager.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"

#include "catalog/pg_database.h"

#include "postmaster/bgworker.h"

#include "storage/ipc.h"

#include "utils/guc.h"

#include "pglogical.h"
#include "pg_logical_conflict.h"

PG_MODULE_MAGIC;

static const struct config_enum_entry PGLogicalConflictResolvers[] = {
	{"error", PGLOGICAL_RESOLVE_ERROR, false},
	{"apply_remote", PGLOGICAL_RESOLVE_APPLY_REMOTE, false},
	{"apply_remote", PGLOGICAL_RESOLVE_KEEP_LOCAL, false},
	{"last_update_wins", PGLOGICAL_RESOLVE_LAST_UPDATE_WINS, false},
	{"first_update_wins", PGLOGICAL_RESOLVE_FIRST_UPDATE_WINS, false},
	{NULL, 0, false}
};

/*
 * Register the manager bgworker for the given DB. The manager worker will then
 * start the apply workers.
 *
 * Called in postmaster context from _PG_init, and under backend from node join
 * funcions.
 */
static void
pg_logical_manager_register(Oid dboid)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;

	bgw.bgw_flags =	BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_main = NULL;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN,
			 EXTENSION_NAME);
	snprintf(bgw.bgw_function_name, BGW_MAXLEN,
			 "pg_logical_manager_main");
	bgw.bgw_restart_time = 0;
	bgw.bgw_notify_pid = 0;
	snprintf(bgw.bgw_name, BGW_MAXLEN,
			 "pglogical manager");
	bgw.bgw_main_arg = ObjectIdGetDatum(dboid);

	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("Registering worker failed, check prior log messages for details")));
	}
}

/*
 * Static bgworker used for initialization.
 */
void
pglogical_init(Datum main_arg)
{
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;

	BackgroundWorkerInitializeConnection(NULL, NULL);

	StartTransactionCommand();

	/* Run manager worker for every connectable database. */
	rel = heap_open(DatabaseRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database pgdatabase = (Form_pg_database) GETSTRUCT(tup);

		if (pgdatabase->datallowconn)
		{
			elog(DEBUG1, "registering pglogical manager process for database %s",
				 NameStr(pgdatabase->datname));
			pg_logical_manager_register(HeapTupleGetOid(tup));
		}
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	CommitTransactionCommand();

	proc_exit(0);
}



/*
 * Entry point for this module.
 */
void
_PG_init(void)
{
	BackgroundWorker bgw;

		DefineCustomEnumVariable("pglogical.conflict_resolution",
							 gettext_noop("Sets method used for conflict resolution for resolvable conflicts."),
							 NULL,
							 &pglogical_conflict_resolver,
							 PGLOGICAL_RESOLVE_LAST_UPDATE_WINS,
							 PGLogicalConflictResolvers,
							 PGC_SUSET, 0,
							 NULL, NULL, NULL);

	if (IsBinaryUpgrade)
		return;

	bgw.bgw_flags =	BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_main = NULL;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN,
			 EXTENSION_NAME);
	snprintf(bgw.bgw_function_name, BGW_MAXLEN,
			 "pglogical_init");
	bgw.bgw_restart_time = 1;
	bgw.bgw_notify_pid = 0;
	snprintf(bgw.bgw_name, BGW_MAXLEN,
			 "pglogical init");
	bgw.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&bgw);
}
