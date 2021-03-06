/*-------------------------------------------------------------------------
 *
 * utility.c
 *	  Contains functions which control the execution of the POSTGRES utility
 *	  commands.  At one time acted as an interface between the Lisp and C
 *	  systems.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012, Postgres-XC Development Group
 * Portions Copyright (c) 2014-2017, ADB Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/utility.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/toasting.h"
#include "commands/alter.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/collationcmds.h"
#include "commands/conversioncmds.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/discard.h"
#include "commands/event_trigger.h"
#include "commands/explain.h"
#include "commands/extension.h"
#include "commands/matview.h"
#include "commands/lockcmds.h"
#include "commands/policy.h"
#include "commands/portalcmds.h"
#include "commands/prepare.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/seclabel.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "postmaster/bgwriter.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteRemove.h"
#include "storage/fd.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/syscache.h"


#ifdef ADB
#if defined(AGTM)
#include "access/transam.h"
#endif
#include "agtm/agtm.h"
#include "catalog/index.h"
#include "nodes/nodes.h"
#include "optimizer/pgxcplan.h"
#include "pgxc/barrier.h"
#include "pgxc/execRemote.h"
#include "pgxc/groupmgr.h"
#include "pgxc/locator.h"
#include "pgxc/nodemgr.h"
#include "pgxc/pgxc.h"
#include "pgxc/poolutils.h"
#include "pgxc/poolmgr.h"
#include "pgxc/xc_maintenance_mode.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "intercomm/inter-comm.h"

typedef struct RemoteUtilityContext
{
	bool				sentToRemote;
	bool				force_autocommit;
	bool				is_temp;
	RemoteQueryExecType	exec_type;
	Node			   *stmt;
	const char		   *query;
	ExecNodes		   *nodes;
} RemoteUtilityContext;

static void ExecRemoteUtilityStmt(RemoteUtilityContext *context);
static bool IsAlterTableStmtRedistribution(AlterTableStmt *atstmt);
static RemoteQueryExecType ExecUtilityFindNodes(ObjectType objectType, Oid relid, bool *is_temp);
static RemoteQueryExecType ExecUtilityFindNodesRelkind(Oid relid, bool *is_temp);
static RemoteQueryExecType GetNodesForCommentUtility(CommentStmt *stmt, bool *is_temp);
static RemoteQueryExecType GetNodesForRulesUtility(RangeVar *relation, bool *is_temp);
static void DropStmtPreTreatment(DropStmt *stmt, const char *queryString,
					bool sentToRemote, bool *is_temp,
					RemoteQueryExecType *exec_type);
static bool IsStmtAllowedInLockedMode(Node *parsetree, const char *queryString);
#endif

/* Hook for plugins to get control in ProcessUtility() */
ProcessUtility_hook_type ProcessUtility_hook = NULL;

/* local function declarations */
static void ProcessUtilitySlow(Node *parsetree,
				   const char *queryString,
				   ProcessUtilityContext context,
				   ParamListInfo params,
				   DestReceiver *dest,
#ifdef ADB
				   bool sentToRemote,
#endif
				   char *completionTag);
#ifdef ADB
static void ExecDropStmt(DropStmt *stmt, bool isTopLevel, const char *queryString, bool sentToRemote);
#else
static void ExecDropStmt(DropStmt *stmt, bool isTopLevel);
#endif

/*
 * CommandIsReadOnly: is an executable query read-only?
 *
 * This is a much stricter test than we apply for XactReadOnly mode;
 * the query must be *in truth* read-only, because the caller wishes
 * not to do CommandCounterIncrement for it.
 *
 * Note: currently no need to support Query nodes here
 */
bool
CommandIsReadOnly(Node *parsetree)
{
	if (IsA(parsetree, PlannedStmt))
	{
		PlannedStmt *stmt = (PlannedStmt *) parsetree;

		switch (stmt->commandType)
		{
			case CMD_SELECT:
				if (stmt->rowMarks != NIL)
					return false;		/* SELECT FOR [KEY] UPDATE/SHARE */
				else if (stmt->hasModifyingCTE)
					return false;		/* data-modifying CTE */
				else
					return true;
			case CMD_UPDATE:
			case CMD_INSERT:
			case CMD_DELETE:
				return false;
			default:
				elog(WARNING, "unrecognized commandType: %d",
					 (int) stmt->commandType);
				break;
		}
	}
	/* For now, treat all utility commands as read/write */
	return false;
}

/*
 * check_xact_readonly: is a utility command read-only?
 *
 * Here we use the loose rules of XactReadOnly mode: no permanent effects
 * on the database are allowed.
 */
static void
check_xact_readonly(Node *parsetree)
{
	/* Only perform the check if we have a reason to do so. */
	if (!XactReadOnly && !IsInParallelMode())
		return;

	/*
	 * Note: Commands that need to do more complicated checking are handled
	 * elsewhere, in particular COPY and plannable statements do their own
	 * checking.  However they should all call PreventCommandIfReadOnly or
	 * PreventCommandIfParallelMode to actually throw the error.
	 */

	switch (nodeTag(parsetree))
	{
		case T_AlterDatabaseStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDomainStmt:
		case T_AlterFunctionStmt:
		case T_AlterRoleStmt:
		case T_AlterRoleSetStmt:
		case T_AlterObjectDependsStmt:
		case T_AlterObjectSchemaStmt:
		case T_AlterOwnerStmt:
		case T_AlterOperatorStmt:
		case T_AlterSeqStmt:
		case T_AlterTableMoveAllStmt:
		case T_AlterTableStmt:
		case T_RenameStmt:
		case T_CommentStmt:
		case T_DefineStmt:
		case T_CreateCastStmt:
		case T_CreateEventTrigStmt:
		case T_AlterEventTrigStmt:
		case T_CreateConversionStmt:
		case T_CreatedbStmt:
		case T_CreateDomainStmt:
		case T_CreateFunctionStmt:
		case T_CreateRoleStmt:
		case T_IndexStmt:
		case T_CreatePLangStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_AlterOpFamilyStmt:
		case T_RuleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateStmt:
		case T_CreateTableAsStmt:
		case T_RefreshMatViewStmt:
		case T_CreateTableSpaceStmt:
		case T_CreateTransformStmt:
		case T_CreateTrigStmt:
		case T_CompositeTypeStmt:
		case T_CreateEnumStmt:
		case T_CreateRangeStmt:
		case T_AlterEnumStmt:
		case T_ViewStmt:
		case T_DropStmt:
		case T_DropdbStmt:
		case T_DropTableSpaceStmt:
		case T_DropRoleStmt:
		case T_GrantStmt:
		case T_GrantRoleStmt:
		case T_AlterDefaultPrivilegesStmt:
		case T_TruncateStmt:
		case T_DropOwnedStmt:
		case T_ReassignOwnedStmt:
		case T_AlterTSDictionaryStmt:
		case T_AlterTSConfigurationStmt:
		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_AlterUserMappingStmt:
		case T_DropUserMappingStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_CreateForeignTableStmt:
		case T_ImportForeignSchemaStmt:
		case T_SecLabelStmt:
			PreventCommandIfReadOnly(CreateCommandTag(parsetree));
			PreventCommandIfParallelMode(CreateCommandTag(parsetree));
			break;
		default:
			/* do nothing */
			break;
	}
}

/*
 * PreventCommandIfReadOnly: throw error if XactReadOnly
 *
 * This is useful mainly to ensure consistency of the error message wording;
 * most callers have checked XactReadOnly for themselves.
 */
void
PreventCommandIfReadOnly(const char *cmdname)
{
	if (XactReadOnly)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s in a read-only transaction",
						cmdname)));
}

/*
 * PreventCommandIfParallelMode: throw error if current (sub)transaction is
 * in parallel mode.
 *
 * This is useful mainly to ensure consistency of the error message wording;
 * most callers have checked IsInParallelMode() for themselves.
 */
void
PreventCommandIfParallelMode(const char *cmdname)
{
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s during a parallel operation",
						cmdname)));
}

/*
 * PreventCommandDuringRecovery: throw error if RecoveryInProgress
 *
 * The majority of operations that are unsafe in a Hot Standby slave
 * will be rejected by XactReadOnly tests.  However there are a few
 * commands that are allowed in "read-only" xacts but cannot be allowed
 * in Hot Standby mode.  Those commands should call this function.
 */
void
PreventCommandDuringRecovery(const char *cmdname)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s during recovery",
						cmdname)));
}

/*
 * CheckRestrictedOperation: throw error for hazardous command if we're
 * inside a security restriction context.
 *
 * This is needed to protect session-local state for which there is not any
 * better-defined protection mechanism, such as ownership.
 */
static void
CheckRestrictedOperation(const char *cmdname)
{
	if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		/* translator: %s is name of a SQL command, eg PREPARE */
			 errmsg("cannot execute %s within security-restricted operation",
					cmdname)));
}


/*
 * ProcessUtility
 *		general utility function invoker
 *
 *	parsetree: the parse tree for the utility statement
 *	queryString: original source text of command
 *	context: identifies source of statement (toplevel client command,
 *		non-toplevel client command, subcommand of a larger utility command)
 *	params: parameters to use during execution
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * Notes: as of PG 8.4, caller MUST supply a queryString; it is not
 * allowed anymore to pass NULL.  (If you really don't have source text,
 * you can pass a constant string, perhaps "(query not available)".)
 *
 * completionTag is only set nonempty if we want to return a nondefault status.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
ProcessUtility(Node *parsetree,
			   const char *queryString,
			   ProcessUtilityContext context,
			   ParamListInfo params,
			   DestReceiver *dest,
#ifdef ADB
			   bool sentToRemote,
#endif /* ADB */
			   char *completionTag)
{
	Assert(queryString != NULL);	/* required as of 8.4 */

	/*
	 * We provide a function hook variable that lets loadable plugins get
	 * control when ProcessUtility is called.  Such a plugin would normally
	 * call standard_ProcessUtility().
	 */
	if (ProcessUtility_hook)
		(*ProcessUtility_hook) (parsetree, queryString,
								context, params,
								dest,
#ifdef ADB
								sentToRemote,
#endif /* ADB */
								completionTag);
#ifdef ADBMGRD
	else if(IsMgrNode(parsetree))
		mgr_ProcessUtility(parsetree, queryString, context, params, dest, completionTag);
#endif /* ADBMGRD */
	else
		standard_ProcessUtility(parsetree, queryString,
								context, params,
								dest,
#ifdef ADB
								sentToRemote,
#endif /* ADB */
								completionTag);
}

/*
 * standard_ProcessUtility itself deals only with utility commands for
 * which we do not provide event trigger support.  Commands that do have
 * such support are passed down to ProcessUtilitySlow, which contains the
 * necessary infrastructure for such triggers.
 *
 * This division is not just for performance: it's critical that the
 * event trigger code not be invoked when doing START TRANSACTION for
 * example, because we might need to refresh the event trigger cache,
 * which requires being in a valid transaction.
 */
void
standard_ProcessUtility(Node *parsetree,
						const char *queryString,
						ProcessUtilityContext context,
						ParamListInfo params,
						DestReceiver *dest,
#ifdef ADB
						bool sentToRemote,
#endif /* ADB */
						char *completionTag)
{
	bool		isTopLevel = (context == PROCESS_UTILITY_TOPLEVEL);

#ifdef ADB

	Relation	vacuum_rel = NULL;

	RemoteUtilityContext utilityContext = {
							sentToRemote,
							false,
							false,
							EXEC_ON_ALL_NODES,
							NULL,
							queryString,
							NULL
						};
	/*
	 * For more detail see comments in function pgxc_lock_for_backup.
	 *
	 * Cosider the following scenario:
	 * Imagine a two cordinator cluster CO1, CO2
	 * Suppose a client connected to CO1 issues select pgxc_lock_for_backup()
	 * Now assume that a client connected to CO2 issues a create table
	 * select pgxc_lock_for_backup() would try to acquire the advisory lock
	 * in exclusive mode, whereas create table would try to acquire the same
	 * lock in shared mode. Both these requests will always try acquire the
	 * lock in the same order i.e. they would both direct the request first to
	 * CO1 and then to CO2. One of the two requests would therefore pass
	 * and the other would fail.
	 *
	 * Consider another scenario:
	 * Suppose we have a two cooridnator cluster CO1 and CO2
	 * Assume one client connected to each coordinator
	 * Further assume one client starts a transaction
	 * and issues a DDL. This is an unfinished transaction.
	 * Now assume the second client issues
	 * select pgxc_lock_for_backup()
	 * This request would fail because the unfinished transaction
	 * would already hold the advisory lock.
	 */
	if (IsCoordMaster() && IsNormalProcessingMode())
	{
		/* Is the statement a prohibited one? */
		if (!IsStmtAllowedInLockedMode(parsetree, queryString))
			pgxc_lock_for_utility_stmt(parsetree);
	}
#endif

	check_xact_readonly(parsetree);

	if (completionTag)
		completionTag[0] = '\0';

	switch (nodeTag(parsetree))
	{
			/*
			 * ******************** transactions ********************
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
						/*
						 * START TRANSACTION, as defined by SQL99: Identical
						 * to BEGIN.  Same code for both.
						 */
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
						{
							ListCell   *lc;

							BeginTransactionBlock();
							foreach(lc, stmt->options)
							{
								DefElem    *item = (DefElem *) lfirst(lc);

								if (strcmp(item->defname, "transaction_isolation") == 0)
									SetPGVariable("transaction_isolation",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_read_only") == 0)
									SetPGVariable("transaction_read_only",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_deferrable") == 0)
									SetPGVariable("transaction_deferrable",
												  list_make1(item->arg),
												  true);
#if defined(AGTM)
								else if (strcmp(item->defname, "least_xid_is") == 0)
								{
									A_Const			*con;
									TransactionId	 least_xid;

									AssertArg(IsA(item->arg, A_Const));
									con = (A_Const *) (item->arg);
									AssertArg(nodeTag(&con->val) == T_Integer);
									least_xid = (TransactionId)intVal(&con->val);

									AdjustTransactionId(least_xid);
								}
#endif
							}
						}
						break;

					case TRANS_STMT_COMMIT:
						if (!EndTransactionBlock())
						{
							/* report unsuccessful commit in completionTag */
							if (completionTag)
								strcpy(completionTag, "ROLLBACK");
						}
						break;

					case TRANS_STMT_PREPARE:
						PreventCommandDuringRecovery("PREPARE TRANSACTION");
						if (!PrepareTransactionBlock(stmt->gid))
						{
							/* report unsuccessful commit in completionTag */
							if (completionTag)
								strcpy(completionTag, "ROLLBACK");
						}
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						PreventTransactionChain(isTopLevel, "COMMIT PREPARED");
						PreventCommandDuringRecovery("COMMIT PREPARED");
#if defined(ADB) || defined(AGTM)
#ifdef ADB
						SetCurrentXactPhase2();
#endif
						FinishPreparedTransactionExt(stmt->gid, true, stmt->missing_ok);
#ifdef ADB
						SetCurrentXactPhase1();
#endif
#else
						FinishPreparedTransaction(stmt->gid, true);
#endif
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						PreventTransactionChain(isTopLevel, "ROLLBACK PREPARED");
						PreventCommandDuringRecovery("ROLLBACK PREPARED");
#if defined(ADB) || defined(AGTM)
#ifdef ADB
						SetCurrentXactPhase2();
#endif
						FinishPreparedTransactionExt(stmt->gid, false, stmt->missing_ok);
#ifdef ADB
						SetCurrentXactPhase1();
#endif
#else
						FinishPreparedTransaction(stmt->gid, false);
#endif
						break;

					case TRANS_STMT_ROLLBACK:
						UserAbortTransactionBlock();
						break;

					case TRANS_STMT_SAVEPOINT:
						{
							ListCell   *cell;
							char	   *name = NULL;

#ifdef ADB
							ereport(ERROR,
									(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
									 (errmsg("SAVEPOINT is not yet supported."))));
#endif

							RequireTransactionChain(isTopLevel, "SAVEPOINT");

							foreach(cell, stmt->options)
							{
								DefElem    *elem = lfirst(cell);

								if (strcmp(elem->defname, "savepoint_name") == 0)
									name = strVal(elem->arg);
							}

							Assert(PointerIsValid(name));

							DefineSavepoint(name);
						}
						break;

					case TRANS_STMT_RELEASE:
						RequireTransactionChain(isTopLevel, "RELEASE SAVEPOINT");
						ReleaseSavepoint(stmt->options);
						break;

					case TRANS_STMT_ROLLBACK_TO:
						RequireTransactionChain(isTopLevel, "ROLLBACK TO SAVEPOINT");
						RollbackToSavepoint(stmt->options);

						/*
						 * CommitTransactionCommand is in charge of
						 * re-defining the savepoint again
						 */
						break;
				}
			}
			break;

			/*
			 * Portal (cursor) manipulation
			 *
			 * Note: DECLARE CURSOR is processed mostly as a SELECT, and
			 * therefore what we will get here is a PlannedStmt not a bare
			 * DeclareCursorStmt.
			 */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				if (stmt->utilityStmt == NULL ||
					!IsA(stmt->utilityStmt, DeclareCursorStmt))
					elog(ERROR, "non-DECLARE CURSOR PlannedStmt passed to ProcessUtility");
				PerformCursorOpen(stmt, params, queryString, isTopLevel);
			}
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				CheckRestrictedOperation("CLOSE");
				PerformPortalClose(stmt->portalname);
			}
			break;

		case T_FetchStmt:
			PerformPortalFetch((FetchStmt *) parsetree, dest,
							   completionTag);
			break;

		case T_DoStmt:
			ExecuteDoStmt((DoStmt *) parsetree);
			break;

		case T_CreateTableSpaceStmt:
#ifdef ADB
			if (IsCoordMaster())
#endif
			/* no event triggers for global objects */
			PreventTransactionChain(isTopLevel, "CREATE TABLESPACE");
			CreateTableSpace((CreateTableSpaceStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_DropTableSpaceStmt:
#ifdef ADB
			/* Allow this to be run inside transaction block on remote nodes */
			if (IsCoordMaster())
#endif
			/* no event triggers for global objects */
			PreventTransactionChain(isTopLevel, "DROP TABLESPACE");
			DropTableSpace((DropTableSpaceStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_AlterTableSpaceOptionsStmt:
			/* no event triggers for global objects */
			AlterTableSpaceOptions((AlterTableSpaceOptionsStmt *) parsetree);
#ifdef ADB
			utilityContext.force_autocommit = true;
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_TruncateStmt:
#ifdef ADB
			/*
			 * In Postgres-XC, TRUNCATE needs to be launched to remote nodes
			 * before AFTER triggers. As this needs an internal control it is
			 * managed by this function internally.
			 */
			ExecuteTruncate((TruncateStmt *) parsetree, queryString);
#else
			ExecuteTruncate((TruncateStmt *) parsetree);
#endif
			break;

		case T_CopyStmt:
			{
				uint64		processed;

				DoCopy((CopyStmt *) parsetree, queryString, &processed);
				if (completionTag)
					snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
							 "COPY " UINT64_FORMAT, processed);
			}
			break;

		case T_PrepareStmt:
			CheckRestrictedOperation("PREPARE");
			PrepareQuery((PrepareStmt *) parsetree, queryString);
			break;

		case T_ExecuteStmt:
			ExecuteQuery((ExecuteStmt *) parsetree, NULL,
						 queryString, params,
						 dest, completionTag);
			break;

		case T_DeallocateStmt:
			CheckRestrictedOperation("DEALLOCATE");
			DeallocateQuery((DeallocateStmt *) parsetree);
			break;

		case T_GrantRoleStmt:
			/* no event triggers for global objects */
			GrantRole((GrantRoleStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_CreatedbStmt:
#ifdef ADB
			if (IsCoordMaster())
#endif
			/* no event triggers for global objects */
			PreventTransactionChain(isTopLevel, "CREATE DATABASE");
			createdb((CreatedbStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_AlterDatabaseStmt:
			/* no event triggers for global objects */
			AlterDatabase((AlterDatabaseStmt *) parsetree, isTopLevel);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_AlterDatabaseSetStmt:
			/* no event triggers for global objects */
			AlterDatabaseSet((AlterDatabaseSetStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_DropdbStmt:
			{
				DropdbStmt *stmt = (DropdbStmt *) parsetree;
#ifdef ADB
				/* Allow this to be run inside transaction block on remote nodes */
				if (IsCoordMaster())
#endif
				/* no event triggers for global objects */
				PreventTransactionChain(isTopLevel, "DROP DATABASE");
				dropdb(stmt->dbname, stmt->missing_ok);
#ifdef ADB
				/* Clean connections before dropping a database on local node */
				if (IsCoordMaster())
				{
					RemoteUtilityContext rcontext;
					char				*query;

					DropDBCleanConnection(stmt->dbname);
					/* Clean also remote Coordinators */
					query = psprintf("CLEAN CONNECTION TO ALL FOR DATABASE %s;", stmt->dbname);

					rcontext.sentToRemote = sentToRemote;
					rcontext.force_autocommit = true;
					rcontext.is_temp = false;
					rcontext.exec_type = EXEC_ON_COORDS;
					rcontext.stmt = NULL;
					rcontext.query = query;
					rcontext.nodes = NULL;
					ExecRemoteUtilityStmt(&rcontext);
					pfree(query);
				}
#endif

#ifdef ADB
				if (IsCoordMaster())
					agtms_DropSequenceByDataBase(stmt->dbname);
				ExecRemoteUtilityStmt(&utilityContext);
#endif
			}
			break;

			/* Query-level asynchronous notification */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				PreventCommandDuringRecovery("NOTIFY");
				Async_Notify(stmt->conditionname, stmt->payload);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				PreventCommandDuringRecovery("LISTEN");
				CheckRestrictedOperation("LISTEN");
				Async_Listen(stmt->conditionname);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				PreventCommandDuringRecovery("UNLISTEN");
				CheckRestrictedOperation("UNLISTEN");
				if (stmt->conditionname)
					Async_Unlisten(stmt->conditionname);
				else
					Async_UnlistenAll();
			}
			break;

		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				closeAllVfds(); /* probably not necessary... */
				/* Allowed names are restricted if you're not superuser */
				load_file(stmt->filename, !superuser());
#ifdef ADB
				utilityContext.exec_type = EXEC_ON_DATANODES;
				ExecRemoteUtilityStmt(&utilityContext);
#endif
			}
			break;

		case T_ClusterStmt:
			/* we choose to allow this during "read only" transactions */
			PreventCommandDuringRecovery("CLUSTER");
			/* forbidden in parallel mode due to CommandIsReadOnly */
			cluster((ClusterStmt *) parsetree, isTopLevel);
#ifdef ADB
			if (IsCoordMaster())
			{
				ClusterStmt *stmt = (ClusterStmt *) parsetree;
				bool need_remote = true;

				if (stmt->relation)
				{
					Relation rel = relation_openrv(stmt->relation, NoLock);
					need_remote = (RelationGetLocInfo(rel) != NULL);
					relation_close(rel, NoLock);
				}
				if(need_remote)
				{
					utilityContext.force_autocommit = true;
					utilityContext.exec_type = EXEC_ON_DATANODES;
					ExecRemoteUtilityStmt(&utilityContext);
				}
			}
#endif
			break;

		case T_VacuumStmt:
			{
				VacuumStmt *stmt = (VacuumStmt *) parsetree;

				/* we choose to allow this during "read only" transactions */
				PreventCommandDuringRecovery((stmt->options & VACOPT_VACUUM) ?
											 "VACUUM" : "ANALYZE");
#ifdef ADB
				if (IsCoordMaster() && stmt->relation)
				{
					vacuum_rel = heap_openrv_extended(stmt->relation, AccessShareLock, true);

					if(vacuum_rel)
					{
						if (RELKIND_MATVIEW!=RelationGetForm(vacuum_rel)->relkind &&
							RelationGetLocInfo(vacuum_rel))
						{
							/*
							 * We have to run the command on nodes before Coordinator because
							 * vacuum() pops active snapshot and we can not send it to nodes
							 */
							utilityContext.force_autocommit = true;
							utilityContext.exec_type = EXEC_ON_DATANODES;
							ExecRemoteUtilityStmt(&utilityContext);
						}

						relation_close(vacuum_rel, AccessShareLock);
					}
				}

#endif /* ADB */
				/* forbidden in parallel mode due to CommandIsReadOnly */
				ExecVacuum(stmt, isTopLevel);
			}
			break;

		case T_ExplainStmt:
			ExplainQuery((ExplainStmt *) parsetree, queryString, params, dest);
			break;

		case T_AlterSystemStmt:
			PreventTransactionChain(isTopLevel, "ALTER SYSTEM");
			AlterSystemSetConfigFile((AlterSystemStmt *) parsetree);
			break;

		case T_VariableSetStmt:
			ExecSetVariableStmt((VariableSetStmt *) parsetree, isTopLevel);
#ifdef ADB
			/* Let the pooler manage the statement */
			if (IsCoordMaster())
			{
				VariableSetStmt *stmt = (VariableSetStmt *) parsetree;
				/*
				 * If command is local and we are not in a transaction block do NOT
				 * send this query to backend nodes, it is just bypassed by the backend.
				 * And we can't send "grammar".
				 */
				if (stmt->name != NULL && strcmp(stmt->name, "grammar") == 0)
				{
					/* nothing to do */
				} else if (stmt->is_local)
				{
					if (IsTransactionBlock())
					{
						if (PoolManagerSetCommand(POOL_CMD_LOCAL_SET, queryString) < 0)
							elog(ERROR, "Postgres-XC: ERROR SET query");
					}
				}
				else
				{
					if (PoolManagerSetCommand(POOL_CMD_GLOBAL_SET, queryString) < 0)
						elog(ERROR, "Postgres-XC: ERROR SET query");
				}
			}
#endif
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name, dest);
			}
			break;

		case T_DiscardStmt:
			/* should we allow DISCARD PLANS? */
			CheckRestrictedOperation("DISCARD");
			DiscardCommand((DiscardStmt *) parsetree, isTopLevel);
#ifdef ADB
			/*
			 * Discard objects for all the sessions possible.
			 * For example, temporary tables are created on all Datanodes
			 * and Coordinators.
			 */
			utilityContext.force_autocommit = true;
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_CreateEventTrigStmt:
			/* no event triggers on event triggers */
			CreateEventTrigger((CreateEventTrigStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_AlterEventTrigStmt:
			/* no event triggers on event triggers */
			AlterEventTrigger((AlterEventTrigStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

			/*
			 * ******************************** ROLE statements ****
			 */
		case T_CreateRoleStmt:
			/* no event triggers for global objects */
			CreateRole((CreateRoleStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_AlterRoleStmt:
			/* no event triggers for global objects */
			AlterRole((AlterRoleStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_AlterRoleSetStmt:
			/* no event triggers for global objects */
			AlterRoleSet((AlterRoleSetStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_DropRoleStmt:
			/* no event triggers for global objects */
			DropRole((DropRoleStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_ReassignOwnedStmt:
			/* no event triggers for global objects */
			ReassignOwnedObjects((ReassignOwnedStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_LockStmt:

			/*
			 * Since the lock would just get dropped immediately, LOCK TABLE
			 * outside a transaction block is presumed to be user error.
			 */
			RequireTransactionChain(isTopLevel, "LOCK TABLE");
			/* forbidden in parallel mode due to CommandIsReadOnly */
			LockTableCommand((LockStmt *) parsetree);
#ifdef ADB
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_ConstraintsSetStmt:
			WarnNoTransactionChain(isTopLevel, "SET CONSTRAINTS");
			AfterTriggerSetState((ConstraintsSetStmt *) parsetree);
#ifdef ADB
			/*
			 * Let the pooler manage the statement, SET CONSTRAINT can just be used
			 * inside a transaction block, hence it has no effect outside that, so use
			 * it as a local one.
			 */
			if (IsTransactionBlock())
				ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

		case T_CheckPointStmt:
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("must be superuser to do CHECKPOINT")));

			/*
			 * You might think we should have a PreventCommandDuringRecovery()
			 * here, but we interpret a CHECKPOINT command during recovery as
			 * a request for a restartpoint instead. We allow this since it
			 * can be a useful way of reducing switchover time when using
			 * various forms of replication.
			 */
			RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_WAIT |
							  (RecoveryInProgress() ? 0 : CHECKPOINT_FORCE));
#ifdef ADB
			utilityContext.force_autocommit = true;
			utilityContext.exec_type = EXEC_ON_DATANODES;
			ExecRemoteUtilityStmt(&utilityContext);
#endif
			break;

#ifdef ADB
#if 0
		case T_BarrierStmt:
			RequestBarrier(((BarrierStmt *) parsetree)->id, completionTag);
			break;
#endif

		/*
		 * Node DDL is an operation local to Coordinator.
		 * In case of a new node being created in the cluster,
		 * it is necessary to create this node on all the Coordinators independently.
		 */
		case T_AlterNodeStmt:
			PgxcNodeAlter((AlterNodeStmt *) parsetree);
			break;

		case T_CreateNodeStmt:
			PgxcNodeCreate((CreateNodeStmt *) parsetree);
			break;

		case T_DropNodeStmt:
			PgxcNodeRemove((DropNodeStmt *) parsetree);
			break;

		case T_CreateGroupStmt:
			PgxcGroupCreate((CreateGroupStmt *) parsetree);
			break;

		case T_DropGroupStmt:
			PgxcGroupRemove((DropGroupStmt *) parsetree);
			break;
#endif

		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parsetree;
#ifdef ADB
				bool send_to_remote = true;
#endif /* ADB */

				/* we choose to allow this during "read only" transactions */
				PreventCommandDuringRecovery("REINDEX");
				/* forbidden in parallel mode due to CommandIsReadOnly */
				switch (stmt->kind)
				{
					case REINDEX_OBJECT_INDEX:
						ReindexIndex(stmt->relation, stmt->options);
						break;
					case REINDEX_OBJECT_TABLE:
						ReindexTable(stmt->relation, stmt->options);
						break;
					case REINDEX_OBJECT_SCHEMA:
					case REINDEX_OBJECT_SYSTEM:
					case REINDEX_OBJECT_DATABASE:

						/*
						 * This cannot run inside a user transaction block; if
						 * we were inside a transaction, then its commit- and
						 * start-transaction-command calls would not have the
						 * intended effect!
						 */
						PreventTransactionChain(isTopLevel,
												(stmt->kind == REINDEX_OBJECT_SCHEMA) ? "REINDEX SCHEMA" :
												(stmt->kind == REINDEX_OBJECT_SYSTEM) ? "REINDEX SYSTEM" :
												"REINDEX DATABASE");
						ReindexMultipleTables(stmt->name, stmt->kind, stmt->options);
						break;
					default:
						elog(ERROR, "unrecognized object type: %d",
							 (int) stmt->kind);
						break;
				}
#ifdef ADB
				if (stmt->kind == REINDEX_OBJECT_INDEX ||
					stmt->kind == REINDEX_OBJECT_TABLE)
				{
					Relation rel = relation_openrv(stmt->relation, NoLock);
					if (RelationUsesLocalBuffers(rel))
						send_to_remote = false;
					relation_close(rel, NoLock);
				}
				if(send_to_remote)
				{
					utilityContext.force_autocommit = (stmt->kind == REINDEX_OBJECT_DATABASE || stmt->kind == REINDEX_OBJECT_SCHEMA);
					ExecRemoteUtilityStmt(&utilityContext);
				}
#endif
			}
			break;

			/*
			 * The following statements are supported by Event Triggers only
			 * in some cases, so we "fast path" them in the other cases.
			 */

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

#ifdef ADB
				if (IsCoordMaster())
				{
					RemoteQueryExecType	remoteExecType = EXEC_ON_ALL_NODES;
					bool				is_temp = false;
					bool				is_temp2;

					/* Launch GRANT on Coordinator if object is a sequence */
					if ((stmt->objtype == ACL_OBJECT_RELATION &&
						 stmt->targtype == ACL_TARGET_OBJECT))
					{
						/*
						 * In case object is a relation, differenciate the case
						 * of a sequence, a view and a table
						 */
						ListCell   *cell;
						/* Check the list of objects */
						bool		first = true;
						RemoteQueryExecType type_local = remoteExecType;

						foreach (cell, stmt->objects)
						{
							RangeVar   *relvar = (RangeVar *) lfirst(cell);
							Oid			relid = RangeVarGetRelid(relvar, NoLock, true);

							/* Skip if object does not exist */
							if (!OidIsValid(relid))
								continue;

							remoteExecType = ExecUtilityFindNodesRelkind(relid, &is_temp2);

							/* Check if object node type corresponds to the first one */
							if (first)
							{
								type_local = remoteExecType;
								is_temp = is_temp2;
								first = false;
							}
							else
							{
								if (type_local != remoteExecType ||
									is_temp != is_temp2)
									ereport(ERROR,
											(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
											 errmsg("PGXC does not support GRANT on multiple object types"),
											 errdetail("Grant VIEW/TABLE with separate queries")));
							}
						}
					}
					if(!is_temp)
					{
						utilityContext.exec_type = remoteExecType;
						utilityContext.is_temp = is_temp;
						ExecRemoteUtilityStmt(&utilityContext);
					}
				}
#endif
				if (EventTriggerSupportsGrantObjectType(stmt->objtype))
					ProcessUtilitySlow(parsetree, queryString,
									   context, params,
									   dest,
#ifdef ADB
									   sentToRemote,
#endif
									   completionTag);
				else
					ExecuteGrantStmt((GrantStmt *) parsetree);
			}
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->removeType))
					ProcessUtilitySlow(parsetree, queryString,
									   context, params,
									   dest,
#ifdef ADB
									   sentToRemote,
#endif
									   completionTag);
				else
#ifdef ADB
					ExecDropStmt(stmt, isTopLevel, queryString, sentToRemote);
#else
					ExecDropStmt(stmt, isTopLevel);
#endif
			}
			break;

		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;

#ifdef ADB
				if (IsCoordMaster())
				{
					RemoteQueryExecType	exec_type;
					bool				is_temp = false;

					/* Try to use the object relation if possible */
					if (stmt->relation)
					{
						/*
						 * When a relation is defined, it is possible that this object does
						 * not exist but an IF EXISTS clause might be used. So we do not do
						 * any error check here but block the access to remote nodes to
						 * this object as it does not exisy
						 */
						Oid relid = RangeVarGetRelid(stmt->relation, NoLock, true);

						if (OidIsValid(relid))
							exec_type = ExecUtilityFindNodes(stmt->renameType,
															 relid,
															 &is_temp);
						else
							exec_type = EXEC_ON_NONE;
					} else
					{
						exec_type = ExecUtilityFindNodes(stmt->renameType,
														 InvalidOid,
														 &is_temp);
					}

					if(!is_temp)
					{
						utilityContext.exec_type = exec_type;
						utilityContext.is_temp = is_temp;
						ExecRemoteUtilityStmt(&utilityContext);
					}
				}
#endif

				if (EventTriggerSupportsObjectType(stmt->renameType))
					ProcessUtilitySlow(parsetree, queryString,
									   context, params,
									   dest,
#ifdef ADB
									   sentToRemote,
#endif
									   completionTag);
				else
					ExecRenameStmt(stmt);
			}
			break;

		case T_AlterObjectDependsStmt:
			{
				AlterObjectDependsStmt *stmt = (AlterObjectDependsStmt *) parsetree;
#ifdef ADB
				if (IsCoordMaster())
				{
					RemoteQueryExecType exec_type;
					bool				is_temp = false;

					/* Try to use the object relation if possible */
					if (stmt->relation)
					{
						/*
						 * When a relation is defined, it is possible that this object does
						 * not exist but an IF EXISTS clause might be used. So we do not do
						 * any error check here but block the access to remote nodes to
						 * this object as it does not exisy
						 */
						Oid relid = RangeVarGetRelid(stmt->relation, NoLock, true);

						if (OidIsValid(relid))
						{
							exec_type = ExecUtilityFindNodes(stmt->objectType,
															 relid,
															 &is_temp);
						} else
							exec_type = EXEC_ON_NONE;
					} else
					{
						exec_type = ExecUtilityFindNodes(stmt->objectType,
														 InvalidOid,
														 &is_temp);
					}

					if(!is_temp)
					{
						utilityContext.exec_type = exec_type;
						utilityContext.is_temp = is_temp;
						ExecRemoteUtilityStmt(&utilityContext);
					}
					/* ADBQ TODO this at AGTM */
				}
#endif

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(parsetree, queryString,
									   context, params,
									   dest,
#ifdef ADB
									   sentToRemote,
#endif
									   completionTag);
				else
					ExecAlterObjectDependsStmt(stmt, NULL);
			}
			break;

		case T_AlterObjectSchemaStmt:
			{
				AlterObjectSchemaStmt *stmt = (AlterObjectSchemaStmt *) parsetree;
#ifdef ADB
				Oid oid;
				if (IsCoordMaster())
				{
					RemoteQueryExecType	exec_type;
					bool				is_temp = false;

					/* Try to use the object relation if possible */
					if (stmt->relation)
					{
						/*
						 * When a relation is defined, it is possible that this object does
						 * not exist but an IF EXISTS clause might be used. So we do not do
						 * any error check here but block the access to remote nodes to
						 * this object as it does not exisy
						 */
						Oid relid = RangeVarGetRelid(stmt->relation, NoLock, true);

						if (OidIsValid(relid))
						{
							oid = relid;
							exec_type = ExecUtilityFindNodes(stmt->objectType,
															 relid,
															 &is_temp);
						} else
							exec_type = EXEC_ON_NONE;
 					} else
					{
						exec_type = ExecUtilityFindNodes(stmt->objectType,
														 InvalidOid,
														 &is_temp);
					}

					if(!is_temp)
					{
						utilityContext.exec_type = exec_type;
						utilityContext.is_temp = is_temp;
						ExecRemoteUtilityStmt(&utilityContext);
					}

					/* execute alter sequecne (set schema)	on agtm */
					if (stmt->objectType == OBJECT_SEQUENCE)
					{
						Relation	targetrelation;
						char		*seqName = NULL;
						char		*databaseName = NULL;
						char		*schemaName = NULL;

						targetrelation = relation_open(oid, AccessExclusiveLock);

						seqName = RelationGetRelationName(targetrelation);
						databaseName = get_database_name(targetrelation->rd_node.dbNode);
						schemaName = get_namespace_name(RelationGetNamespace(targetrelation));

						agtm_RenameSequence(seqName, databaseName,
							schemaName, stmt->newschema, T_RENAME_SCHEMA);

						relation_close(targetrelation, NoLock);
					}
				}
#endif

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(parsetree, queryString,
									   context, params,
									   dest,
#ifdef ADB
									   sentToRemote,
#endif
									   completionTag);
				else
					ExecAlterObjectSchemaStmt(stmt, NULL);
			}
			break;

		case T_AlterOwnerStmt:
			{
				AlterOwnerStmt *stmt = (AlterOwnerStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(parsetree, queryString,
									   context, params,
									   dest,
#ifdef ADB
									   sentToRemote,
#endif
									   completionTag);
				else
					ExecAlterOwnerStmt(stmt);

#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
			}
			break;

		case T_CommentStmt:
			{
				CommentStmt *stmt = (CommentStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(parsetree, queryString,
									   context, params,
									   dest,
#ifdef ADB
									   sentToRemote,
#endif
									   completionTag);
				else
					CommentObject((CommentStmt *) parsetree);
#ifdef ADB
				/* Comment objects depending on their object and temporary types */
				if (IsCoordMaster())
				{
					bool is_temp = false;
					RemoteQueryExecType exec_type = GetNodesForCommentUtility(stmt, &is_temp);
					if(!is_temp)
					{
						utilityContext.exec_type = exec_type;
						utilityContext.is_temp = is_temp;
						ExecRemoteUtilityStmt(&utilityContext);
					}
				}
#endif
				break;
			}

		case T_SecLabelStmt:
			{
				SecLabelStmt *stmt = (SecLabelStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(parsetree, queryString,
									   context, params,
									   dest,
#ifdef ADB
									   sentToRemote,
#endif
									   completionTag);
				else
					ExecSecLabelStmt(stmt);
				break;
			}

#ifdef ADB
		case T_RemoteQuery:
			Assert(IS_PGXC_COORDINATOR);

			if (!IsConnFromCoord())
				(void) ExecInterXactUtility((RemoteQuery *) parsetree, GetCurrentInterXactState());
			break;

		case T_CleanConnStmt:
			Assert(IS_PGXC_COORDINATOR);
			CleanConnection((CleanConnStmt *) parsetree);

			utilityContext.force_autocommit = true;
			utilityContext.exec_type = EXEC_ON_COORDS;
			ExecRemoteUtilityStmt(&utilityContext);
			break;
#endif

		default:
			/* All other statement types have event trigger support */
#ifdef ADB
			elog(DEBUG1,"Query String:%s, SendToRemote=%d,CompletionTag=%s\n",queryString,sentToRemote,completionTag);
#endif /* ADB */
			ProcessUtilitySlow(parsetree, queryString,
							   context, params,
							   dest,
#ifdef ADB
							   sentToRemote,
#endif
							   completionTag);
			break;
	}
}

/*
 * The "Slow" variant of ProcessUtility should only receive statements
 * supported by the event triggers facility.  Therefore, we always
 * perform the trigger support calls if the context allows it.
 */
static void
ProcessUtilitySlow(Node *parsetree,
				   const char *queryString,
				   ProcessUtilityContext context,
				   ParamListInfo params,
				   DestReceiver *dest,
#ifdef ADB
				   bool sentToRemote,
#endif
				   char *completionTag)
{
	bool		isTopLevel = (context == PROCESS_UTILITY_TOPLEVEL);
	bool		isCompleteQuery = (context <= PROCESS_UTILITY_QUERY);
	bool		needCleanup;
	bool		commandCollected = false;
	ObjectAddress address;
	ObjectAddress secondaryObject = InvalidObjectAddress;
#ifdef ADB
	RemoteUtilityContext utilityContext = {
							sentToRemote,
							false,
							false,
							EXEC_ON_ALL_NODES,
							NULL,
							queryString,
							NULL
						};
#endif

	/* All event trigger calls are done only when isCompleteQuery is true */
	needCleanup = isCompleteQuery && EventTriggerBeginCompleteQuery();

	/* PG_TRY block is to ensure we call EventTriggerEndCompleteQuery */
	PG_TRY();
	{
		if (isCompleteQuery)
			EventTriggerDDLCommandStart(parsetree);

		switch (nodeTag(parsetree))
		{
				/*
				 * relation and attribute manipulation
				 */
			case T_CreateSchemaStmt:
#ifdef ADB
				CreateSchemaCommand((CreateSchemaStmt *) parsetree,
									queryString, sentToRemote);
#else
				CreateSchemaCommand((CreateSchemaStmt *) parsetree,
									queryString);
#endif

				/*
				 * EventTriggerCollectSimpleCommand called by
				 * CreateSchemaCommand
				 */
				commandCollected = true;
				break;

			case T_CreateStmt:
			case T_CreateForeignTableStmt:
				{
					List	   *stmts;
					ListCell   *l;
#ifdef ADB
					bool		is_temp = false;
					Node	   *transformed_stmt = NULL;
#endif

					/* Run parse analysis ... */
					stmts = transformCreateStmt((CreateStmt *) parsetree,
												queryString ADB_ONLY_COMMA_ARG(&transformed_stmt));
#ifdef ADB
					if (IsCoordMaster())
					{
						/*
						 * Scan the list of objects.
						 * Temporary tables are created on coordinator only.
						 * Non-temporary objects are created on all nodes.
						 * In case temporary and non-temporary objects are mized return an error.
						 */
						bool	is_first = true;

						foreach(l, stmts)
						{
							Node	   *stmt = (Node *) lfirst(l);

							if (IsA(stmt, CreateStmt))
							{
								CreateStmt *stmt_loc = (CreateStmt *) stmt;
								bool is_object_temp = stmt_loc->relation->relpersistence == RELPERSISTENCE_TEMP;

								if (is_object_temp && stmt_loc->distributeby)
									ereport(ERROR,
											(errcode(ERRCODE_SYNTAX_ERROR),
											 errmsg("temporary table not support distribute by")));

								if (is_first)
								{
									is_first = false;
									if (is_object_temp)
										is_temp = true;
								}
								else
								{
									if (is_object_temp != is_temp)
										ereport(ERROR,
												(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
												 errmsg("CREATE not supported for TEMP and non-TEMP objects"),
												 errdetail("You should separate TEMP and non-TEMP objects")));
								}
							}
							else if (IsA(stmt, CreateForeignTableStmt))
							{
								/* There are no temporary foreign tables */
								if (is_first)
								{
									is_first = false;
								}
								else
								{
									if (!is_temp)
										ereport(ERROR,
												(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
												 errmsg("CREATE not supported for TEMP and non-TEMP objects"),
												 errdetail("You should separate TEMP and non-TEMP objects")));
								}
							}
						}
					}

					/*
					 * Add a RemoteQuery node for a query at top level on a remote
					 * Coordinator, if not already done so
					 */
					if (!sentToRemote && !is_temp)
					{
						if (transformed_stmt)
							stmts = AddRemoteParseTree(stmts, queryString, transformed_stmt, EXEC_ON_ALL_NODES, is_temp);
						else
							stmts = AddRemoteParseTree(stmts, queryString, parsetree, EXEC_ON_ALL_NODES, is_temp);
					}
#endif

					/* ... and do it */
					foreach(l, stmts)
					{
						Node	   *stmt = (Node *) lfirst(l);

						if (IsA(stmt, CreateStmt))
						{
							Datum		toast_options;
							static char *validnsps[] = HEAP_RELOPT_NAMESPACES;

							/* Create the table itself */
							address = DefineRelation((CreateStmt *) stmt,
													 RELKIND_RELATION,
													 InvalidOid, NULL);
							EventTriggerCollectSimpleCommand(address,
															 secondaryObject,
															 stmt);

							/*
							 * Let NewRelationCreateToastTable decide if this
							 * one needs a secondary relation too.
							 */
							CommandCounterIncrement();

							/*
							 * parse and validate reloptions for the toast
							 * table
							 */
							toast_options = transformRelOptions((Datum) 0,
											  ((CreateStmt *) stmt)->options,
																"toast",
																validnsps,
																true,
																false);
							(void) heap_reloptions(RELKIND_TOASTVALUE,
												   toast_options,
												   true);

							NewRelationCreateToastTable(address.objectId,
														toast_options);
						}
						else if (IsA(stmt, CreateForeignTableStmt))
						{
							/* Create the table itself */
							address = DefineRelation((CreateStmt *) stmt,
													 RELKIND_FOREIGN_TABLE,
													 InvalidOid, NULL);
							CreateForeignTable((CreateForeignTableStmt *) stmt,
											   address.objectId);
							EventTriggerCollectSimpleCommand(address,
															 secondaryObject,
															 stmt);
						}
						else
						{
							/*
							 * Recurse for anything else.  Note the recursive
							 * call will stash the objects so created into our
							 * event trigger context.
							 */
							ProcessUtility(stmt,
										   queryString,
										   PROCESS_UTILITY_SUBCOMMAND,
										   params,
										   None_Receiver,
#ifdef ADB
										   true,
#endif /* ADB */
										   NULL);
						}

						/* Need CCI between commands */
						if (lnext(l) != NULL)
							CommandCounterIncrement();
					}

					/*
					 * The multiple commands generated here are stashed
					 * individually, so disable collection below.
					 */
					commandCollected = true;
				}
				break;

			case T_AlterTableStmt:
				{
					AlterTableStmt *atstmt = (AlterTableStmt *) parsetree;
					Oid			relid;
					List	   *stmts;
					ListCell   *l;
					LOCKMODE	lockmode;

					/*
					 * Figure out lock mode, and acquire lock.  This also does
					 * basic permissions checks, so that we won't wait for a
					 * lock on (for example) a relation on which we have no
					 * permissions.
					 */
					lockmode = AlterTableGetLockLevel(atstmt->cmds);
					relid = AlterTableLookupRelation(atstmt, lockmode);

					if (OidIsValid(relid))
					{
						/* Run parse analysis ... */
						stmts = transformAlterTableStmt(relid, atstmt,
														queryString);
#ifdef ADB
						/*
						 * Add a RemoteQuery node for a query at top level on a remote
						 * Coordinator, if not already done so
						 */
						if (!sentToRemote)
						{
							bool is_temp = false;
							RemoteQueryExecType exec_type;
							Oid relid = RangeVarGetRelid(atstmt->relation,
														 NoLock, true);

							if (OidIsValid(relid))
							{
								exec_type = ExecUtilityFindNodes(atstmt->relkind,
																 relid,
																 &is_temp);
								/*
								 * If the AlterTableStmt self will only update the catalog
								 * pgxc_node, the RemoteQuery added for the AlterTableStmt
								 * should only be done on coordinators.
								 */
								if (!is_temp)
								{
									if (atstmt->relkind == OBJECT_TABLE &&
										IsAlterTableStmtRedistribution(atstmt))
										exec_type = EXEC_ON_COORDS;

									stmts = AddRemoteParseTree(stmts, queryString, parsetree, exec_type, is_temp);
								}
							}
						}
#endif

						/* ... ensure we have an event trigger context ... */
						EventTriggerAlterTableStart(parsetree);
						EventTriggerAlterTableRelid(relid);

						/* ... and do it */
						foreach(l, stmts)
						{
							Node	   *stmt = (Node *) lfirst(l);

							if (IsA(stmt, AlterTableStmt))
							{
								/* Do the table alteration proper */
								AlterTable(relid, lockmode,
										   (AlterTableStmt *) stmt);
							}
							else
							{
								/*
								 * Recurse for anything else.  If we need to
								 * do so, "close" the current complex-command
								 * set, and start a new one at the bottom;
								 * this is needed to ensure the ordering of
								 * queued commands is consistent with the way
								 * they are executed here.
								 */
								EventTriggerAlterTableEnd();
								ProcessUtility(stmt,
											   queryString,
											   PROCESS_UTILITY_SUBCOMMAND,
											   params,
											   None_Receiver,
#ifdef ADB
											   true,
#endif
											   NULL);
								EventTriggerAlterTableStart(parsetree);
								EventTriggerAlterTableRelid(relid);
							}

							/* Need CCI between commands */
							if (lnext(l) != NULL)
								CommandCounterIncrement();
						}

						/* done */
						EventTriggerAlterTableEnd();
					}
					else
						ereport(NOTICE,
						  (errmsg("relation \"%s\" does not exist, skipping",
								  atstmt->relation->relname)));
				}

				/* ALTER TABLE stashes commands internally */
				commandCollected = true;
				break;

			case T_AlterDomainStmt:
				{
					AlterDomainStmt *stmt = (AlterDomainStmt *) parsetree;

					/*
					 * Some or all of these functions are recursive to cover
					 * inherited things, so permission checks are done there.
					 */
					switch (stmt->subtype)
					{
						case 'T':		/* ALTER DOMAIN DEFAULT */

							/*
							 * Recursively alter column default for table and,
							 * if requested, for descendants
							 */
							address =
								AlterDomainDefault(stmt->typeName,
												   stmt->def);
							break;
						case 'N':		/* ALTER DOMAIN DROP NOT NULL */
							address =
								AlterDomainNotNull(stmt->typeName,
												   false);
							break;
						case 'O':		/* ALTER DOMAIN SET NOT NULL */
							address =
								AlterDomainNotNull(stmt->typeName,
												   true);
							break;
						case 'C':		/* ADD CONSTRAINT */
							address =
								AlterDomainAddConstraint(stmt->typeName,
														 stmt->def,
														 &secondaryObject);
							break;
						case 'X':		/* DROP CONSTRAINT */
							address =
								AlterDomainDropConstraint(stmt->typeName,
														  stmt->name,
														  stmt->behavior,
														  stmt->missing_ok);
							break;
						case 'V':		/* VALIDATE CONSTRAINT */
							address =
								AlterDomainValidateConstraint(stmt->typeName,
															  stmt->name);
							break;
						default:		/* oops */
							elog(ERROR, "unrecognized alter domain type: %d",
								 (int) stmt->subtype);
							break;
					}

#ifdef ADB
					ExecRemoteUtilityStmt(&utilityContext);
#endif
				}
				break;

				/*
				 * ************* object creation / destruction **************
				 */
			case T_DefineStmt:
				{
					DefineStmt *stmt = (DefineStmt *) parsetree;

					switch (stmt->kind)
					{
						case OBJECT_AGGREGATE:
							address =
								DefineAggregate(stmt->defnames, stmt->args,
												stmt->oldstyle,
											  stmt->definition, queryString);
							break;
						case OBJECT_OPERATOR:
							Assert(stmt->args == NIL);
							address = DefineOperator(stmt->defnames,
													 stmt->definition);
							break;
						case OBJECT_TYPE:
							Assert(stmt->args == NIL);
							address = DefineType(stmt->defnames,
												 stmt->definition);
							break;
						case OBJECT_TSPARSER:
							Assert(stmt->args == NIL);
							address = DefineTSParser(stmt->defnames,
													 stmt->definition);
							break;
						case OBJECT_TSDICTIONARY:
							Assert(stmt->args == NIL);
							address = DefineTSDictionary(stmt->defnames,
														 stmt->definition);
							break;
						case OBJECT_TSTEMPLATE:
							Assert(stmt->args == NIL);
							address = DefineTSTemplate(stmt->defnames,
													   stmt->definition);
							break;
						case OBJECT_TSCONFIGURATION:
							Assert(stmt->args == NIL);
							address = DefineTSConfiguration(stmt->defnames,
															stmt->definition,
															&secondaryObject);
							break;
						case OBJECT_COLLATION:
							Assert(stmt->args == NIL);
							address = DefineCollation(stmt->defnames,
													  stmt->definition);
							break;
						default:
							elog(ERROR, "unrecognized define stmt type: %d",
								 (int) stmt->kind);
							break;
					}

#ifdef ADB
					ExecRemoteUtilityStmt(&utilityContext);
#endif
				}
				break;

			case T_IndexStmt:	/* CREATE INDEX */
				{
					IndexStmt  *stmt = (IndexStmt *) parsetree;
					Oid			relid;
					LOCKMODE	lockmode;
#ifdef ADB
					bool		is_temp = false;
					RemoteQueryExecType	exec_type = EXEC_ON_ALL_NODES;

					if (stmt->concurrent)
					{
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("PGXC does not support concurrent INDEX yet"),
								 errdetail("The feature is not currently supported")));
					}

					/* INDEX on a temporary table cannot use 2PC at commit */
					relid = RangeVarGetRelid(stmt->relation, NoLock, true);

					if (OidIsValid(relid))
						exec_type = ExecUtilityFindNodes(OBJECT_INDEX, relid, &is_temp);
					else
						exec_type = EXEC_ON_NONE;
#endif

					if (stmt->concurrent)
						PreventTransactionChain(isTopLevel,
												"CREATE INDEX CONCURRENTLY");

					/*
					 * Look up the relation OID just once, right here at the
					 * beginning, so that we don't end up repeating the name
					 * lookup later and latching onto a different relation
					 * partway through.  To avoid lock upgrade hazards, it's
					 * important that we take the strongest lock that will
					 * eventually be needed here, so the lockmode calculation
					 * needs to match what DefineIndex() does.
					 */
					lockmode = stmt->concurrent ? ShareUpdateExclusiveLock
						: ShareLock;
					relid =
						RangeVarGetRelidExtended(stmt->relation, lockmode,
												 false, false,
												 RangeVarCallbackOwnsRelation,
												 NULL);

					/* Run parse analysis ... */
					stmt = transformIndexStmt(relid, stmt, queryString);

					/* ... and do it */
					EventTriggerAlterTableStart(parsetree);
					address =
						DefineIndex(relid,		/* OID of heap relation */
									stmt,
									InvalidOid, /* no predefined OID */
									false,		/* is_alter_table */
									true,		/* check_rights */
									false,		/* skip_build */
									false);		/* quiet */

					/*
					 * Add the CREATE INDEX node itself to stash right away;
					 * if there were any commands stashed in the ALTER TABLE
					 * code, we need them to appear after this one.
					 */
					EventTriggerCollectSimpleCommand(address, secondaryObject,
													 parsetree);
					commandCollected = true;
					EventTriggerAlterTableEnd();

#ifdef ADB
					if (!stmt->isconstraint && !is_temp)
					{
						utilityContext.force_autocommit = stmt->concurrent;
						utilityContext.exec_type = exec_type;
						utilityContext.is_temp = is_temp;
						utilityContext.stmt = (Node *) stmt;
						ExecRemoteUtilityStmt(&utilityContext);
					}
#endif
				}
				break;

			case T_CreateExtensionStmt:
				address = CreateExtension((CreateExtensionStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterExtensionStmt:
				address = ExecAlterExtensionStmt((AlterExtensionStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterExtensionContentsStmt:
				address = ExecAlterExtensionContentsStmt((AlterExtensionContentsStmt *) parsetree,
														 &secondaryObject);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateFdwStmt:
				address = CreateForeignDataWrapper((CreateFdwStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterFdwStmt:
				address = AlterForeignDataWrapper((AlterFdwStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateForeignServerStmt:
				address = CreateForeignServer((CreateForeignServerStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterForeignServerStmt:
				address = AlterForeignServer((AlterForeignServerStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateUserMappingStmt:
				address = CreateUserMapping((CreateUserMappingStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterUserMappingStmt:
				address = AlterUserMapping((AlterUserMappingStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_DropUserMappingStmt:
				RemoveUserMapping((DropUserMappingStmt *) parsetree);
				/* no commands stashed for DROP */
				commandCollected = true;
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_ImportForeignSchemaStmt:
				ImportForeignSchema((ImportForeignSchemaStmt *) parsetree);
				/* commands are stashed inside ImportForeignSchema */
				commandCollected = true;
				break;

			case T_CompositeTypeStmt:	/* CREATE TYPE (composite) */
				{
					CompositeTypeStmt *stmt = (CompositeTypeStmt *) parsetree;

					address = DefineCompositeType(stmt->typevar,
												  stmt->coldeflist);
#ifdef ADB
					ExecRemoteUtilityStmt(&utilityContext);
#endif
				}
				break;

			case T_CreateEnumStmt:		/* CREATE TYPE AS ENUM */
				address = DefineEnum((CreateEnumStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateRangeStmt:		/* CREATE TYPE AS RANGE */
				address = DefineRange((CreateRangeStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterEnumStmt:		/* ALTER TYPE (enum) */
				address = AlterEnum((AlterEnumStmt *) parsetree, isTopLevel);
#ifdef ADB
				/*
				 * In this case force autocommit, this transaction cannot be launched
				 * inside a transaction block.
				 */
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_ViewStmt:	/* CREATE VIEW */
				EventTriggerAlterTableStart(parsetree);
				address = DefineView((ViewStmt *) parsetree, queryString);
				EventTriggerCollectSimpleCommand(address, secondaryObject,
												 parsetree);
				/* stashed internally */
				commandCollected = true;
				EventTriggerAlterTableEnd();
#ifdef ADB
				/*
				 * temporary view is only defined locally, if not,
				 * defined on all coordinators.
				 */
				if (((ViewStmt *) parsetree)->view->relpersistence != RELPERSISTENCE_TEMP)
				{
					/* sometimes force be a temporary view, we need test again */
					Relation rel = heap_open(address.objectId, NoLock);
					bool need_remote = RelationUsesLocalBuffers(rel) ? false:true;
					heap_close(rel, NoLock);
					if(need_remote)
					{
						utilityContext.exec_type = EXEC_ON_COORDS;
						utilityContext.stmt = (Node *) parsetree;
						ExecRemoteUtilityStmt(&utilityContext);
					}
				}
#endif
				break;

			case T_CreateFunctionStmt:	/* CREATE FUNCTION */
				address = CreateFunction((CreateFunctionStmt *) parsetree, queryString);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterFunctionStmt:	/* ALTER FUNCTION */
				address = AlterFunction((AlterFunctionStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_RuleStmt:	/* CREATE RULE */
				address = DefineRule((RuleStmt *) parsetree, queryString);
#ifdef ADB
				if (IsCoordMaster())
				{
					RemoteQueryExecType	exec_type;
					bool				is_temp;

					exec_type = GetNodesForRulesUtility(((RuleStmt *) parsetree)->relation,
														&is_temp);
					if (!is_temp)
					{
						utilityContext.exec_type = exec_type;
						utilityContext.is_temp = is_temp;
						ExecRemoteUtilityStmt(&utilityContext);
					}
				}
#endif
				break;

			case T_CreateSeqStmt:
				address = DefineSequence((CreateSeqStmt *) parsetree);
#ifdef ADB
				if (IS_PGXC_COORDINATOR)
				{
					CreateSeqStmt *stmt = (CreateSeqStmt *) parsetree;

					/* In case this query is related to a SERIAL execution, just bypass */
					if (!stmt->is_serial &&
						stmt->sequence->relpersistence != RELPERSISTENCE_TEMP)
					{
						utilityContext.is_temp = false;
						utilityContext.stmt = (Node *) parsetree;
						ExecRemoteUtilityStmt(&utilityContext);
					}
				}
#endif
				break;

			case T_AlterSeqStmt:
				address = AlterSequence((AlterSeqStmt *) parsetree);
#ifdef ADB
				if (IS_PGXC_COORDINATOR)
				{
					AlterSeqStmt *stmt = (AlterSeqStmt *) parsetree;

					/* In case this query is related to a SERIAL execution, just bypass */
					if (!stmt->is_serial)
					{
						bool		  is_temp;
						RemoteQueryExecType exec_type;
						Oid 				relid = RangeVarGetRelid(stmt->sequence, NoLock, true);

						if (!OidIsValid(relid))
							break;

						exec_type = ExecUtilityFindNodes(OBJECT_SEQUENCE,
														 relid,
														 &is_temp);

						if (!is_temp)
						{
							utilityContext.exec_type = exec_type;
							utilityContext.is_temp = is_temp;
							ExecRemoteUtilityStmt(&utilityContext);
						}
					}
				}
#endif
				break;

			case T_CreateTableAsStmt:
				address = ExecCreateTableAs((CreateTableAsStmt *) parsetree,
										 queryString, params, completionTag);
#ifdef ADB
				/* Send CREATE MATERIALIZED VIEW command to all coordinators. */
				/* see pg_rewrite_query */
				Assert(((CreateTableAsStmt *) parsetree)->relkind == OBJECT_MATVIEW);
				if (!ObjectAddressIsInValid(address))
				{
					if (!((CreateTableAsStmt *) parsetree)->into->skipData && !IsConnFromCoord())
						pgxc_send_matview_data(((CreateTableAsStmt *) parsetree)->into->rel,
												queryString);
					else
					{
						utilityContext.exec_type = EXEC_ON_COORDS;
						ExecRemoteUtilityStmt(&utilityContext);
					}
				}
#endif /* ADB */
				break;

			case T_RefreshMatViewStmt:

				/*
				 * REFRSH CONCURRENTLY executes some DDL commands internally.
				 * Inhibit DDL command collection here to avoid those commands
				 * from showing up in the deparsed command queue.  The refresh
				 * command itself is queued, which is enough.
				 */
				EventTriggerInhibitCommandCollection();
				PG_TRY();
				{
					address = ExecRefreshMatView((RefreshMatViewStmt *) parsetree,
										 queryString, params, completionTag);
				}
				PG_CATCH();
				{
					EventTriggerUndoInhibitCommandCollection();
					PG_RE_THROW();
				}
				PG_END_TRY();
				EventTriggerUndoInhibitCommandCollection();

#ifdef ADB
				Assert(IS_PGXC_COORDINATOR);
				/* Send REFRESH MATERIALIZED VIEW command and the data to be populated
				 * to all coordinators.
				 */
				if (!((RefreshMatViewStmt *)parsetree)->skipData && !IsConnFromCoord())
					pgxc_send_matview_data(((RefreshMatViewStmt *)parsetree)->relation,
											queryString);
				else
				{
					utilityContext.exec_type = EXEC_ON_COORDS;
					ExecRemoteUtilityStmt(&utilityContext);
				}
#endif /* ADB */
				break;

			case T_CreateTrigStmt:
				address = CreateTrigger((CreateTrigStmt *) parsetree,
										queryString, InvalidOid, InvalidOid,
										InvalidOid, InvalidOid, false);
#ifdef ADB
				if (IS_PGXC_COORDINATOR)
				{
					CreateTrigStmt 	   *stmt = (CreateTrigStmt *) parsetree;
					RemoteQueryExecType	exec_type;
					bool				is_temp;

					exec_type = ExecUtilityFindNodes(OBJECT_TABLE,
													 RangeVarGetRelid(stmt->relation, NoLock, false),
													 &is_temp);
					if(!is_temp)
					{
						utilityContext.exec_type = exec_type;
						utilityContext.is_temp = is_temp;
						ExecRemoteUtilityStmt(&utilityContext);
					}
				}
#endif
				break;

			case T_CreatePLangStmt:
				address = CreateProceduralLanguage((CreatePLangStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateDomainStmt:
				address = DefineDomain((CreateDomainStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateConversionStmt:
				address = CreateConversionCommand((CreateConversionStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateCastStmt:
				address = CreateCast((CreateCastStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateOpClassStmt:
				DefineOpClass((CreateOpClassStmt *) parsetree);
				/* command is stashed in DefineOpClass */
				commandCollected = true;
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateOpFamilyStmt:
				address = DefineOpFamily((CreateOpFamilyStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreateTransformStmt:
				address = CreateTransform((CreateTransformStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterOpFamilyStmt:
				AlterOpFamily((AlterOpFamilyStmt *) parsetree);
				/* commands are stashed in AlterOpFamily */
				commandCollected = true;
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterTSDictionaryStmt:
				address = AlterTSDictionary((AlterTSDictionaryStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterTSConfigurationStmt:
				AlterTSConfiguration((AlterTSConfigurationStmt *) parsetree);
				/*
				 * Commands are stashed in MakeConfigurationMapping and
				 * DropConfigurationMapping, which are called from
				 * AlterTSConfiguration
				 */
				commandCollected = true;
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterTableMoveAllStmt:
				AlterTableMoveAll((AlterTableMoveAllStmt *) parsetree);
				/* commands are stashed in AlterTableMoveAll */
				commandCollected = true;
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_DropStmt:
#ifdef ADB
				ExecDropStmt((DropStmt *) parsetree, isTopLevel, queryString, sentToRemote);
#else
				ExecDropStmt((DropStmt *) parsetree, isTopLevel);
#endif
				/* no commands stashed for DROP */
				commandCollected = true;
				break;

			case T_RenameStmt:
				address = ExecRenameStmt((RenameStmt *) parsetree);
				break;

			case T_AlterObjectDependsStmt:
				address =
					ExecAlterObjectDependsStmt((AlterObjectDependsStmt *) parsetree,
											   &secondaryObject);
				break;

			case T_AlterObjectSchemaStmt:
				address =
					ExecAlterObjectSchemaStmt((AlterObjectSchemaStmt *) parsetree,
											  &secondaryObject);
				break;

			case T_AlterOwnerStmt:
				address = ExecAlterOwnerStmt((AlterOwnerStmt *) parsetree);
				break;

			case T_AlterOperatorStmt:
				address = AlterOperator((AlterOperatorStmt *) parsetree);
				break;

			case T_CommentStmt:
				address = CommentObject((CommentStmt *) parsetree);
				break;

			case T_GrantStmt:
				ExecuteGrantStmt((GrantStmt *) parsetree);
				/* commands are stashed in ExecGrantStmt_oids */
				commandCollected = true;
				break;

			case T_DropOwnedStmt:
				DropOwnedObjects((DropOwnedStmt *) parsetree);
				/* no commands stashed for DROP */
				commandCollected = true;
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_AlterDefaultPrivilegesStmt:
				ExecAlterDefaultPrivilegesStmt((AlterDefaultPrivilegesStmt *) parsetree);
				EventTriggerCollectAlterDefPrivs((AlterDefaultPrivilegesStmt *) parsetree);
				commandCollected = true;
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			case T_CreatePolicyStmt:	/* CREATE POLICY */
				address = CreatePolicy((CreatePolicyStmt *) parsetree);
				break;

			case T_AlterPolicyStmt:		/* ALTER POLICY */
				address = AlterPolicy((AlterPolicyStmt *) parsetree);
				break;

			case T_SecLabelStmt:
				address = ExecSecLabelStmt((SecLabelStmt *) parsetree);
				break;

			case T_CreateAmStmt:
				address = CreateAccessMethod((CreateAmStmt *) parsetree);
#ifdef ADB
				ExecRemoteUtilityStmt(&utilityContext);
#endif
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(parsetree));
				break;
		}

		/*
		 * Remember the object so that ddl_command_end event triggers have
		 * access to it.
		 */
		if (!commandCollected)
			EventTriggerCollectSimpleCommand(address, secondaryObject,
											 parsetree);

		if (isCompleteQuery)
		{
			EventTriggerSQLDrop(parsetree);
			EventTriggerDDLCommandEnd(parsetree);
		}
	}
	PG_CATCH();
	{
		if (needCleanup)
			EventTriggerEndCompleteQuery();
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (needCleanup)
		EventTriggerEndCompleteQuery();
}

/*
 * Dispatch function for DropStmt
 */
static void
#ifdef ADB
ExecDropStmt(DropStmt *stmt, bool isTopLevel, const char *queryString, bool sentToRemote)
#else
ExecDropStmt(DropStmt *stmt, bool isTopLevel)
#endif
{
#ifdef ADB
	RemoteUtilityContext utilityContext = {
							sentToRemote,
							false,
							false,
							EXEC_ON_ALL_NODES,
							NULL,
							queryString,
							NULL
						};

	HOLD_INTERRUPTS();
#endif /* ADB */
	switch (stmt->removeType)
	{
		case OBJECT_INDEX:
			if (stmt->concurrent)
#ifdef ADB
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PGXC does not support concurrent INDEX yet"),
						 errdetail("The feature is not currently supported")));

#else
				PreventTransactionChain(isTopLevel,
										"DROP INDEX CONCURRENTLY");
#endif
			/* fall through */

		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_FOREIGN_TABLE:
#ifdef ADB
			{
				RemoteQueryExecType	exec_type;
				bool				is_temp = false;

				if (stmt->removeType == OBJECT_MATVIEW)
					exec_type = EXEC_ON_COORDS;
				else
					exec_type = EXEC_ON_ALL_NODES;

				/* Check restrictions on objects dropped */
				DropStmtPreTreatment(stmt, queryString, sentToRemote,
									 &is_temp, &exec_type);
#endif
			RemoveRelations(stmt);
#ifdef ADB
				/* DROP is done depending on the object type */
				if(!is_temp)
				{
					utilityContext.is_temp = is_temp;
					utilityContext.exec_type = exec_type;
					utilityContext.stmt = (Node *) stmt;
					ExecRemoteUtilityStmt(&utilityContext);
				}
			}
#endif

			break;
		default:
#ifdef ADB
			{
				RemoteQueryExecType exec_type = EXEC_ON_ALL_NODES;
				bool				is_temp = false;

				/* Check restrictions on objects dropped */
				DropStmtPreTreatment(stmt, queryString, sentToRemote,
									 &is_temp, &exec_type);
#endif
			RemoveObjects(stmt);
#ifdef ADB
				if (!is_temp)
				{
					utilityContext.exec_type = exec_type;
					utilityContext.is_temp = is_temp;
					ExecRemoteUtilityStmt(&utilityContext);
				}
			}
#endif
			break;
	}
#ifdef ADB
	RESUME_INTERRUPTS();
#endif /* ADB */
}


#ifdef ADB
static bool
IsAlterTableStmtRedistribution(AlterTableStmt *atstmt)
{
	AlterTableCmd	*cmd;
	ListCell		*lcmd;

	AssertArg(atstmt);
	Assert(atstmt->relkind == OBJECT_TABLE);

	foreach (lcmd, atstmt->cmds)
	{
		cmd = (AlterTableCmd *) lfirst(lcmd);
		switch(cmd->subtype)
		{
			/*
			 * Datanodes will not do these kinds of commands, such
			 * as AT_SubCluster, AT_AddNodeList, AT_DeleteNodeList,
			 * see the function AtExecSubCluster, AtExecAddNode and
			 * AtExecDeleteNode, so it is not necessary to send the
			 * AlterTableStmt to datanodes.
			 *
			 * But this kind of command AT_DistributeBy should be
			 * sent to datanodes, as the datanode will delete old
			 * even add new dependency about the AlterTableStmt, see
			 * the function AtExecDistributeBy.
			 */
			case AT_SubCluster:
			case AT_AddNodeList:
			case AT_DeleteNodeList:
				break;
			default:
				return false;
				break;
		}
	}
	return true;
}

/*
 * IsStmtAllowedInLockedMode
 *
 * Allow/Disallow a utility command while cluster is locked
 * A statement will be disallowed if it makes such changes
 * in catalog that are backed up by pg_dump except
 * CREATE NODE that has to be allowed because
 * a new node has to be created while the cluster is still
 * locked for backup
 */
static bool
IsStmtAllowedInLockedMode(Node *parsetree, const char *queryString)
{
#define ALLOW		1
#define DISALLOW	0

	switch (nodeTag(parsetree))
	{
		/* To allow creation of temp tables */
		case T_CreateStmt:					/* CREATE TABLE */
			{
				CreateStmt *stmt = (CreateStmt *) parsetree;
				if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP)
					return ALLOW;
				return DISALLOW;
			}
			break;

		case T_ExecuteStmt:					/*
											 * Prepared statememts can only have
											 * SELECT, INSERT, UPDATE, DELETE,
											 * or VALUES statement, there is no
											 * point stopping EXECUTE.
											 */
		case T_CreateNodeStmt:				/*
											 * This has to be allowed so that the new node
											 * can be created, while the cluster is still
											 * locked for backup
											 */
		case T_DropNodeStmt:				/*
											 * This has to be allowed so that DROP NODE
											 * can be issued to drop a node that has crashed.
											 * Otherwise system would try to acquire a shared
											 * advisory lock on the crashed node.
											 */
		case T_AlterNodeStmt:				/*
											 * This has to be allowed so that ALTER
											 * can be issued to alter a node that has crashed
											 * and may be failed over.
											 * Otherwise system would try to acquire a shared
											 * advisory lock on the crashed node.
											 */
		case T_TransactionStmt:
		case T_PlannedStmt:
		case T_ClosePortalStmt:
		case T_FetchStmt:
		case T_TruncateStmt:
		case T_CopyStmt:
		case T_PrepareStmt:					/*
											 * Prepared statememts can only have
											 * SELECT, INSERT, UPDATE, DELETE,
											 * or VALUES statement, there is no
											 * point stopping PREPARE.
											 */
		case T_DeallocateStmt:				/*
											 * If prepare is allowed the deallocate should
											 * be allowed also
											 */
		case T_DoStmt:
		case T_NotifyStmt:
		case T_ListenStmt:
		case T_UnlistenStmt:
		case T_LoadStmt:
		case T_ClusterStmt:
		case T_VacuumStmt:
		case T_ExplainStmt:
		case T_VariableSetStmt:
		case T_VariableShowStmt:
		case T_DiscardStmt:
		case T_LockStmt:
		case T_ConstraintsSetStmt:
		case T_CheckPointStmt:
		case T_BarrierStmt:
		case T_ReindexStmt:
		case T_RemoteQuery:
		case T_CleanConnStmt:
			return ALLOW;

		default:
			return DISALLOW;
	}
	return DISALLOW;
}

/*
 * Execute a Utility statement on nodes, including Coordinators
 * If the DDL is received from a remote Coordinator,
 * it is not possible to push down DDL to Datanodes
 * as it is taken in charge by the remote Coordinator.
 */
static void
ExecRemoteUtilityStmt(RemoteUtilityContext *context)
{
	RemoteQuery *step;

	Assert(context);

	/* only master-coordinator can do this */
	if (!IsCoordMaster())
		return ;

	/* Return if query is launched on no nodes */
	if (context->exec_type == EXEC_ON_NONE)
		return;

	/* Nothing to be done if this statement has been sent to the nodes */
	if (context->sentToRemote)
		return;

	/* If no Datanodes defined, the query cannot be launched */
	if (NumDataNodes == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("No Datanode defined in cluster"),
				 errhint("You need to define at least 1 Datanode with "
						 "CREATE NODE.")));

	step = makeNode(RemoteQuery);
	if(context->stmt)
	{
		step->sql_node = makeStringInfo();
		saveNode(step->sql_node, context->stmt);
	}
	step->combine_type = COMBINE_TYPE_SAME;
	step->exec_nodes = context->nodes;
	step->sql_statement = pstrdup(context->query);
	step->force_autocommit = context->force_autocommit;
	step->exec_type = context->exec_type;
	step->is_temp = context->is_temp;
	(void) ExecInterXactUtility(step, GetCurrentInterXactState());
	if (step->sql_node)
		pfree(step->sql_node->data);
	pfree(step->sql_statement);
	pfree(step);
}

/*
 * ExecUtilityFindNodes
 *
 * Determine the list of nodes to launch query on.
 * This depends on temporary nature of object and object type.
 * Return also a flag indicating if relation is temporary.
 *
 * If object is a RULE, the object id sent is that of the object to which the
 * rule is applicable.
 */
static RemoteQueryExecType
ExecUtilityFindNodes(ObjectType object_type,
					 Oid object_id,
					 bool *is_temp)
{
	RemoteQueryExecType exec_type;

	switch (object_type)
	{
		case OBJECT_SEQUENCE:
			*is_temp = IsTempTable(object_id);
			exec_type = EXEC_ON_ALL_NODES;
			break;

		/* Triggers are evaluated based on the relation they are defined on */
		case OBJECT_TABLE:
		case OBJECT_TRIGGER:
			/* Do the check on relation kind */
			exec_type = ExecUtilityFindNodesRelkind(object_id, is_temp);
			break;

		/*
		 * Views and rules, both permanent or temporary are created
		 * on Coordinators only.
		 */
		case OBJECT_RULE:
		case OBJECT_VIEW:
			/* Check if object is a temporary view */
			if ((*is_temp = IsTempTable(object_id)))
				exec_type = EXEC_ON_NONE;
			else
				exec_type = EXEC_ON_COORDS;
			break;

		case OBJECT_INDEX:
			/* Check if given index uses temporary tables */
			if ((*is_temp = IsTempTable(object_id)))
				exec_type = EXEC_ON_DATANODES;
			/*
			 * Materialized views and hence index on those are located on
			 * coordinators
			 */
			else if (get_rel_relkind(object_id) == RELKIND_MATVIEW ||
						(get_rel_relkind(object_id) == RELKIND_INDEX &&
							get_rel_relkind(IndexGetRelation(object_id, false)) == RELKIND_MATVIEW))
				exec_type = EXEC_ON_COORDS;
			else
				exec_type = EXEC_ON_ALL_NODES;
			break;

		case OBJECT_MATVIEW:
			/* Materialized views are located only on the coordinators */
			*is_temp = false;
			exec_type = EXEC_ON_COORDS;
			break;

		default:
			*is_temp = false;
			exec_type = EXEC_ON_ALL_NODES;
			break;
	}

	return exec_type;
}

/*
 * ExecUtilityFindNodesRelkind
 *
 * Get node execution and temporary type
 * for given relation depending on its relkind
 */
static RemoteQueryExecType
ExecUtilityFindNodesRelkind(Oid relid, bool *is_temp)
{
	char relkind_str = get_rel_relkind(relid);
	RemoteQueryExecType exec_type;

	switch (relkind_str)
	{
		case RELKIND_SEQUENCE:
			*is_temp = IsTempTable(relid);
			exec_type = EXEC_ON_ALL_NODES;
			break;

		case RELKIND_RELATION:
			*is_temp = IsTempTable(relid);
			exec_type = EXEC_ON_ALL_NODES;
			break;

		case RELKIND_VIEW:
			if ((*is_temp = IsTempTable(relid)))
				exec_type = EXEC_ON_NONE;
			else
				exec_type = EXEC_ON_COORDS;
			break;

		default:
			*is_temp = false;
			exec_type = EXEC_ON_ALL_NODES;
			break;
	}

	return exec_type;
}
#endif

/*
 * UtilityReturnsTuples
 *		Return "true" if this utility statement will send output to the
 *		destination.
 *
 * Generally, there should be a case here for each case in ProcessUtility
 * where "dest" is passed on.
 */
bool
UtilityReturnsTuples(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return false;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return false;		/* not our business to raise error */
				return portal->tupDesc ? true : false;
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return false;		/* not our business to raise error */
				if (entry->plansource->resultDesc)
					return true;
				return false;
			}

		case T_ExplainStmt:
			return true;

		case T_VariableShowStmt:
			return true;

		default:
			return false;
	}
}

/*
 * UtilityTupleDescriptor
 *		Fetch the actual output tuple descriptor for a utility statement
 *		for which UtilityReturnsTuples() previously returned "true".
 *
 * The returned descriptor is created in (or copied into) the current memory
 * context.
 */
TupleDesc
UtilityTupleDescriptor(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return NULL;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return NULL;	/* not our business to raise error */
				return CreateTupleDescCopy(portal->tupDesc);
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return NULL;	/* not our business to raise error */
				return FetchPreparedStatementResultDesc(entry);
			}

		case T_ExplainStmt:
			return ExplainResultDesc((ExplainStmt *) parsetree);

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				return GetPGVariableResultDesc(n->name);
			}

		default:
			return NULL;
	}
}


/*
 * QueryReturnsTuples
 *		Return "true" if this Query will send output to the destination.
 */
#ifdef NOT_USED
bool
QueryReturnsTuples(Query *parsetree)
{
	switch (parsetree->commandType)
	{
		case CMD_SELECT:
			/* returns tuples ... unless it's DECLARE CURSOR */
			if (parsetree->utilityStmt == NULL)
				return true;
			break;
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
			/* the forms with RETURNING return tuples */
			if (parsetree->returningList)
				return true;
			break;
		case CMD_UTILITY:
			return UtilityReturnsTuples(parsetree->utilityStmt);
		case CMD_UNKNOWN:
		case CMD_NOTHING:
			/* probably shouldn't get here */
			break;
	}
	return false;				/* default */
}
#endif


/*
 * UtilityContainsQuery
 *		Return the contained Query, or NULL if there is none
 *
 * Certain utility statements, such as EXPLAIN, contain a plannable Query.
 * This function encapsulates knowledge of exactly which ones do.
 * We assume it is invoked only on already-parse-analyzed statements
 * (else the contained parsetree isn't a Query yet).
 *
 * In some cases (currently, only EXPLAIN of CREATE TABLE AS/SELECT INTO and
 * CREATE MATERIALIZED VIEW), potentially Query-containing utility statements
 * can be nested.  This function will drill down to a non-utility Query, or
 * return NULL if none.
 */
Query *
UtilityContainsQuery(Node *parsetree)
{
	Query	   *qry;

	switch (nodeTag(parsetree))
	{
		case T_ExplainStmt:
			qry = (Query *) ((ExplainStmt *) parsetree)->query;
			Assert(IsA(qry, Query));
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		case T_CreateTableAsStmt:
			qry = (Query *) ((CreateTableAsStmt *) parsetree)->query;
			Assert(IsA(qry, Query));
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		default:
			return NULL;
	}
}


/*
 * AlterObjectTypeCommandTag
 *		helper function for CreateCommandTag
 *
 * This covers most cases where ALTER is used with an ObjectType enum.
 */
static const char *
AlterObjectTypeCommandTag(ObjectType objtype)
{
	const char *tag;

	switch (objtype)
	{
		case OBJECT_AGGREGATE:
			tag = "ALTER AGGREGATE";
			break;
		case OBJECT_ATTRIBUTE:
			tag = "ALTER TYPE";
			break;
		case OBJECT_CAST:
			tag = "ALTER CAST";
			break;
		case OBJECT_COLLATION:
			tag = "ALTER COLLATION";
			break;
		case OBJECT_COLUMN:
			tag = "ALTER TABLE";
			break;
		case OBJECT_CONVERSION:
			tag = "ALTER CONVERSION";
			break;
		case OBJECT_DATABASE:
			tag = "ALTER DATABASE";
			break;
		case OBJECT_DOMAIN:
		case OBJECT_DOMCONSTRAINT:
			tag = "ALTER DOMAIN";
			break;
		case OBJECT_EXTENSION:
			tag = "ALTER EXTENSION";
			break;
		case OBJECT_FDW:
			tag = "ALTER FOREIGN DATA WRAPPER";
			break;
		case OBJECT_FOREIGN_SERVER:
			tag = "ALTER SERVER";
			break;
		case OBJECT_FOREIGN_TABLE:
			tag = "ALTER FOREIGN TABLE";
			break;
		case OBJECT_FUNCTION:
			tag = "ALTER FUNCTION";
			break;
		case OBJECT_INDEX:
			tag = "ALTER INDEX";
			break;
		case OBJECT_LANGUAGE:
			tag = "ALTER LANGUAGE";
			break;
		case OBJECT_LARGEOBJECT:
			tag = "ALTER LARGE OBJECT";
			break;
		case OBJECT_OPCLASS:
			tag = "ALTER OPERATOR CLASS";
			break;
		case OBJECT_OPERATOR:
			tag = "ALTER OPERATOR";
			break;
		case OBJECT_OPFAMILY:
			tag = "ALTER OPERATOR FAMILY";
			break;
		case OBJECT_POLICY:
			tag = "ALTER POLICY";
			break;
		case OBJECT_ROLE:
			tag = "ALTER ROLE";
			break;
		case OBJECT_RULE:
			tag = "ALTER RULE";
			break;
		case OBJECT_SCHEMA:
			tag = "ALTER SCHEMA";
			break;
		case OBJECT_SEQUENCE:
			tag = "ALTER SEQUENCE";
			break;
		case OBJECT_TABLE:
		case OBJECT_TABCONSTRAINT:
			tag = "ALTER TABLE";
			break;
		case OBJECT_TABLESPACE:
			tag = "ALTER TABLESPACE";
			break;
		case OBJECT_TRIGGER:
			tag = "ALTER TRIGGER";
			break;
		case OBJECT_EVENT_TRIGGER:
			tag = "ALTER EVENT TRIGGER";
			break;
		case OBJECT_TSCONFIGURATION:
			tag = "ALTER TEXT SEARCH CONFIGURATION";
			break;
		case OBJECT_TSDICTIONARY:
			tag = "ALTER TEXT SEARCH DICTIONARY";
			break;
		case OBJECT_TSPARSER:
			tag = "ALTER TEXT SEARCH PARSER";
			break;
		case OBJECT_TSTEMPLATE:
			tag = "ALTER TEXT SEARCH TEMPLATE";
			break;
		case OBJECT_TYPE:
			tag = "ALTER TYPE";
			break;
		case OBJECT_VIEW:
			tag = "ALTER VIEW";
			break;
		case OBJECT_MATVIEW:
			tag = "ALTER MATERIALIZED VIEW";
			break;
		default:
			tag = "???";
			break;
	}

	return tag;
}

/*
 * CreateCommandTag
 *		utility to get a string representation of the command operation,
 *		given either a raw (un-analyzed) parsetree or a planned query.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 *
 * NB: all result strings must be shorter than COMPLETION_TAG_BUFSIZE.
 * Also, the result must point at a true constant (permanent storage).
 */
const char *
CreateCommandTag(Node *parsetree)
{
	const char *tag;
#ifdef ADBMGRD
	if(IsMgrNode(parsetree))
		return mgr_CreateCommandTag(parsetree);
#endif
	switch (nodeTag(parsetree))
	{
			/* raw plannable queries */
		case T_InsertStmt:
			tag = "INSERT";
			break;

		case T_DeleteStmt:
			tag = "DELETE";
			break;

		case T_UpdateStmt:
			tag = "UPDATE";
			break;

		case T_SelectStmt:
			tag = "SELECT";
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
						tag = "BEGIN";
						break;

					case TRANS_STMT_START:
						tag = "START TRANSACTION";
						break;

					case TRANS_STMT_COMMIT:
						tag = "COMMIT";
						break;

					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_ROLLBACK_TO:
						tag = "ROLLBACK";
						break;

					case TRANS_STMT_SAVEPOINT:
						tag = "SAVEPOINT";
						break;

					case TRANS_STMT_RELEASE:
						tag = "RELEASE";
						break;

					case TRANS_STMT_PREPARE:
						tag = "PREPARE TRANSACTION";
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						tag = "COMMIT PREPARED";
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						tag = "ROLLBACK PREPARED";
						break;

					default:
						tag = "???";
						break;
				}
			}
			break;

		case T_DeclareCursorStmt:
			tag = "DECLARE CURSOR";
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				if (stmt->portalname == NULL)
					tag = "CLOSE CURSOR ALL";
				else
					tag = "CLOSE CURSOR";
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;

				tag = (stmt->ismove) ? "MOVE" : "FETCH";
			}
			break;

		case T_CreateDomainStmt:
			tag = "CREATE DOMAIN";
			break;

		case T_CreateSchemaStmt:
			tag = "CREATE SCHEMA";
			break;

		case T_CreateStmt:
			tag = "CREATE TABLE";
			break;

		case T_CreateTableSpaceStmt:
			tag = "CREATE TABLESPACE";
			break;

		case T_DropTableSpaceStmt:
			tag = "DROP TABLESPACE";
			break;

		case T_AlterTableSpaceOptionsStmt:
			tag = "ALTER TABLESPACE";
			break;

		case T_CreateExtensionStmt:
			tag = "CREATE EXTENSION";
			break;

		case T_AlterExtensionStmt:
			tag = "ALTER EXTENSION";
			break;

		case T_AlterExtensionContentsStmt:
			tag = "ALTER EXTENSION";
			break;

		case T_CreateFdwStmt:
			tag = "CREATE FOREIGN DATA WRAPPER";
			break;

		case T_AlterFdwStmt:
			tag = "ALTER FOREIGN DATA WRAPPER";
			break;

		case T_CreateForeignServerStmt:
			tag = "CREATE SERVER";
			break;

		case T_AlterForeignServerStmt:
			tag = "ALTER SERVER";
			break;

		case T_CreateUserMappingStmt:
			tag = "CREATE USER MAPPING";
			break;

		case T_AlterUserMappingStmt:
			tag = "ALTER USER MAPPING";
			break;

		case T_DropUserMappingStmt:
			tag = "DROP USER MAPPING";
			break;

		case T_CreateForeignTableStmt:
			tag = "CREATE FOREIGN TABLE";
			break;

		case T_ImportForeignSchemaStmt:
			tag = "IMPORT FOREIGN SCHEMA";
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case OBJECT_TABLE:
					tag = "DROP TABLE";
					break;
				case OBJECT_SEQUENCE:
					tag = "DROP SEQUENCE";
					break;
				case OBJECT_VIEW:
					tag = "DROP VIEW";
					break;
				case OBJECT_MATVIEW:
					tag = "DROP MATERIALIZED VIEW";
					break;
				case OBJECT_INDEX:
					tag = "DROP INDEX";
					break;
				case OBJECT_TYPE:
					tag = "DROP TYPE";
					break;
				case OBJECT_DOMAIN:
					tag = "DROP DOMAIN";
					break;
				case OBJECT_COLLATION:
					tag = "DROP COLLATION";
					break;
				case OBJECT_CONVERSION:
					tag = "DROP CONVERSION";
					break;
				case OBJECT_SCHEMA:
					tag = "DROP SCHEMA";
					break;
				case OBJECT_TSPARSER:
					tag = "DROP TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "DROP TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "DROP TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "DROP TEXT SEARCH CONFIGURATION";
					break;
				case OBJECT_FOREIGN_TABLE:
					tag = "DROP FOREIGN TABLE";
					break;
				case OBJECT_EXTENSION:
					tag = "DROP EXTENSION";
					break;
				case OBJECT_FUNCTION:
					tag = "DROP FUNCTION";
					break;
				case OBJECT_AGGREGATE:
					tag = "DROP AGGREGATE";
					break;
				case OBJECT_OPERATOR:
					tag = "DROP OPERATOR";
					break;
				case OBJECT_LANGUAGE:
					tag = "DROP LANGUAGE";
					break;
				case OBJECT_CAST:
					tag = "DROP CAST";
					break;
				case OBJECT_TRIGGER:
					tag = "DROP TRIGGER";
					break;
				case OBJECT_EVENT_TRIGGER:
					tag = "DROP EVENT TRIGGER";
					break;
				case OBJECT_RULE:
					tag = "DROP RULE";
					break;
				case OBJECT_FDW:
					tag = "DROP FOREIGN DATA WRAPPER";
					break;
				case OBJECT_FOREIGN_SERVER:
					tag = "DROP SERVER";
					break;
				case OBJECT_OPCLASS:
					tag = "DROP OPERATOR CLASS";
					break;
				case OBJECT_OPFAMILY:
					tag = "DROP OPERATOR FAMILY";
					break;
				case OBJECT_POLICY:
					tag = "DROP POLICY";
					break;
				case OBJECT_TRANSFORM:
					tag = "DROP TRANSFORM";
					break;
				case OBJECT_ACCESS_METHOD:
					tag = "DROP ACCESS METHOD";
					break;
				default:
					tag = "???";
			}
			break;

		case T_TruncateStmt:
			tag = "TRUNCATE TABLE";
			break;

		case T_CommentStmt:
			tag = "COMMENT";
			break;

		case T_SecLabelStmt:
			tag = "SECURITY LABEL";
			break;

		case T_CopyStmt:
			tag = "COPY";
			break;

		case T_RenameStmt:
			tag = AlterObjectTypeCommandTag(((RenameStmt *) parsetree)->renameType);
			break;

		case T_AlterObjectDependsStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectDependsStmt *) parsetree)->objectType);
			break;

		case T_AlterObjectSchemaStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectSchemaStmt *) parsetree)->objectType);
			break;

		case T_AlterOwnerStmt:
			tag = AlterObjectTypeCommandTag(((AlterOwnerStmt *) parsetree)->objectType);
			break;

		case T_AlterTableMoveAllStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableMoveAllStmt *) parsetree)->objtype);
			break;

		case T_AlterTableStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableStmt *) parsetree)->relkind);
			break;

		case T_AlterDomainStmt:
			tag = "ALTER DOMAIN";
			break;

		case T_AlterFunctionStmt:
			tag = "ALTER FUNCTION";
			break;

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				tag = (stmt->is_grant) ? "GRANT" : "REVOKE";
			}
			break;

		case T_GrantRoleStmt:
			{
				GrantRoleStmt *stmt = (GrantRoleStmt *) parsetree;

				tag = (stmt->is_grant) ? "GRANT ROLE" : "REVOKE ROLE";
			}
			break;

		case T_AlterDefaultPrivilegesStmt:
			tag = "ALTER DEFAULT PRIVILEGES";
			break;

		case T_DefineStmt:
			switch (((DefineStmt *) parsetree)->kind)
			{
				case OBJECT_AGGREGATE:
					tag = "CREATE AGGREGATE";
					break;
				case OBJECT_OPERATOR:
					tag = "CREATE OPERATOR";
					break;
				case OBJECT_TYPE:
					tag = "CREATE TYPE";
					break;
				case OBJECT_TSPARSER:
					tag = "CREATE TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "CREATE TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "CREATE TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "CREATE TEXT SEARCH CONFIGURATION";
					break;
				case OBJECT_COLLATION:
					tag = "CREATE COLLATION";
					break;
				case OBJECT_ACCESS_METHOD:
					tag = "CREATE ACCESS METHOD";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CompositeTypeStmt:
			tag = "CREATE TYPE";
			break;

		case T_CreateEnumStmt:
			tag = "CREATE TYPE";
			break;

		case T_CreateRangeStmt:
			tag = "CREATE TYPE";
			break;

		case T_AlterEnumStmt:
			tag = "ALTER TYPE";
			break;

		case T_ViewStmt:
			tag = "CREATE VIEW";
			break;

		case T_CreateFunctionStmt:
			tag = "CREATE FUNCTION";
			break;

		case T_IndexStmt:
			tag = "CREATE INDEX";
			break;

		case T_RuleStmt:
			tag = "CREATE RULE";
			break;

		case T_CreateSeqStmt:
			tag = "CREATE SEQUENCE";
			break;

		case T_AlterSeqStmt:
			tag = "ALTER SEQUENCE";
			break;

		case T_DoStmt:
			tag = "DO";
			break;

		case T_CreatedbStmt:
			tag = "CREATE DATABASE";
			break;

		case T_AlterDatabaseStmt:
			tag = "ALTER DATABASE";
			break;

		case T_AlterDatabaseSetStmt:
			tag = "ALTER DATABASE";
			break;

		case T_DropdbStmt:
			tag = "DROP DATABASE";
			break;

		case T_NotifyStmt:
			tag = "NOTIFY";
			break;

		case T_ListenStmt:
			tag = "LISTEN";
			break;

		case T_UnlistenStmt:
			tag = "UNLISTEN";
			break;

		case T_LoadStmt:
			tag = "LOAD";
			break;

		case T_ClusterStmt:
			tag = "CLUSTER";
			break;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->options & VACOPT_VACUUM)
				tag = "VACUUM";
			else
				tag = "ANALYZE";
			break;

		case T_ExplainStmt:
			tag = "EXPLAIN";
			break;

		case T_CreateTableAsStmt:
			switch (((CreateTableAsStmt *) parsetree)->relkind)
			{
				case OBJECT_TABLE:
					if (((CreateTableAsStmt *) parsetree)->is_select_into)
						tag = "SELECT INTO";
					else
						tag = "CREATE TABLE AS";
					break;
				case OBJECT_MATVIEW:
					tag = "CREATE MATERIALIZED VIEW";
					break;
				default:
					tag = "???";
			}
			break;

		case T_RefreshMatViewStmt:
			tag = "REFRESH MATERIALIZED VIEW";
			break;

		case T_AlterSystemStmt:
			tag = "ALTER SYSTEM";
			break;

		case T_VariableSetStmt:
			switch (((VariableSetStmt *) parsetree)->kind)
			{
				case VAR_SET_VALUE:
				case VAR_SET_CURRENT:
				case VAR_SET_DEFAULT:
				case VAR_SET_MULTI:
					tag = "SET";
					break;
				case VAR_RESET:
				case VAR_RESET_ALL:
					tag = "RESET";
					break;
				default:
					tag = "???";
			}
			break;

		case T_VariableShowStmt:
			tag = "SHOW";
			break;

		case T_DiscardStmt:
			switch (((DiscardStmt *) parsetree)->target)
			{
				case DISCARD_ALL:
					tag = "DISCARD ALL";
					break;
				case DISCARD_PLANS:
					tag = "DISCARD PLANS";
					break;
				case DISCARD_TEMP:
					tag = "DISCARD TEMP";
					break;
				case DISCARD_SEQUENCES:
					tag = "DISCARD SEQUENCES";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CreateTransformStmt:
			tag = "CREATE TRANSFORM";
			break;

		case T_CreateTrigStmt:
			tag = "CREATE TRIGGER";
			break;

		case T_CreateEventTrigStmt:
			tag = "CREATE EVENT TRIGGER";
			break;

		case T_AlterEventTrigStmt:
			tag = "ALTER EVENT TRIGGER";
			break;

		case T_CreatePLangStmt:
			tag = "CREATE LANGUAGE";
			break;

		case T_CreateRoleStmt:
			tag = "CREATE ROLE";
			break;

		case T_AlterRoleStmt:
			tag = "ALTER ROLE";
			break;

		case T_AlterRoleSetStmt:
			tag = "ALTER ROLE";
			break;

		case T_DropRoleStmt:
			tag = "DROP ROLE";
			break;

		case T_DropOwnedStmt:
			tag = "DROP OWNED";
			break;

		case T_ReassignOwnedStmt:
			tag = "REASSIGN OWNED";
			break;

		case T_LockStmt:
			tag = "LOCK TABLE";
			break;

		case T_ConstraintsSetStmt:
			tag = "SET CONSTRAINTS";
			break;

		case T_CheckPointStmt:
			tag = "CHECKPOINT";
			break;

#ifdef ADB
		case T_BarrierStmt:
			tag = "BARRIER";
			break;

		case T_AlterNodeStmt:
			tag = "ALTER NODE";
			break;

		case T_CreateNodeStmt:
			tag = "CREATE NODE";
			break;

		case T_DropNodeStmt:
			tag = "DROP NODE";
			break;

		case T_CreateGroupStmt:
			tag = "CREATE NODE GROUP";
			break;

		case T_DropGroupStmt:
			tag = "DROP NODE GROUP";
			break;
#endif

		case T_ReindexStmt:
			tag = "REINDEX";
			break;

		case T_CreateConversionStmt:
			tag = "CREATE CONVERSION";
			break;

		case T_CreateCastStmt:
			tag = "CREATE CAST";
			break;

		case T_CreateOpClassStmt:
			tag = "CREATE OPERATOR CLASS";
			break;

		case T_CreateOpFamilyStmt:
			tag = "CREATE OPERATOR FAMILY";
			break;

		case T_AlterOpFamilyStmt:
			tag = "ALTER OPERATOR FAMILY";
			break;

		case T_AlterOperatorStmt:
			tag = "ALTER OPERATOR";
			break;

		case T_AlterTSDictionaryStmt:
			tag = "ALTER TEXT SEARCH DICTIONARY";
			break;

		case T_AlterTSConfigurationStmt:
			tag = "ALTER TEXT SEARCH CONFIGURATION";
			break;

		case T_CreatePolicyStmt:
			tag = "CREATE POLICY";
			break;

		case T_AlterPolicyStmt:
			tag = "ALTER POLICY";
			break;

		case T_CreateAmStmt:
			tag = "CREATE ACCESS METHOD";
			break;

		case T_PrepareStmt:
			tag = "PREPARE";
			break;

		case T_ExecuteStmt:
			tag = "EXECUTE";
			break;

		case T_DeallocateStmt:
			{
				DeallocateStmt *stmt = (DeallocateStmt *) parsetree;

				if (stmt->name == NULL)
					tag = "DEALLOCATE ALL";
				else
					tag = "DEALLOCATE";
			}
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (stmt->utilityStmt != NULL)
						{
							Assert(IsA(stmt->utilityStmt, DeclareCursorStmt));
							tag = "DECLARE CURSOR";
						}
						else if (stmt->rowMarks != NIL)
						{
							/* not 100% but probably close enough */
							switch (((PlanRowMark *) linitial(stmt->rowMarks))->strength)
							{
								case LCS_FORKEYSHARE:
									tag = "SELECT FOR KEY SHARE";
									break;
								case LCS_FORSHARE:
									tag = "SELECT FOR SHARE";
									break;
								case LCS_FORNOKEYUPDATE:
									tag = "SELECT FOR NO KEY UPDATE";
									break;
								case LCS_FORUPDATE:
									tag = "SELECT FOR UPDATE";
									break;
								default:
									tag = "SELECT";
									break;
							}
						}
						else
							tag = "SELECT";
						break;
					case CMD_UPDATE:
						tag = "UPDATE";
						break;
					case CMD_INSERT:
						tag = "INSERT";
						break;
					case CMD_DELETE:
						tag = "DELETE";
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = "???";
						break;
				}
			}
			break;

			/* parsed-and-rewritten-but-not-planned queries */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (stmt->utilityStmt != NULL)
						{
							Assert(IsA(stmt->utilityStmt, DeclareCursorStmt));
							tag = "DECLARE CURSOR";
						}
						else if (stmt->rowMarks != NIL)
						{
							/* not 100% but probably close enough */
							switch (((RowMarkClause *) linitial(stmt->rowMarks))->strength)
							{
								case LCS_FORKEYSHARE:
									tag = "SELECT FOR KEY SHARE";
									break;
								case LCS_FORSHARE:
									tag = "SELECT FOR SHARE";
									break;
								case LCS_FORNOKEYUPDATE:
									tag = "SELECT FOR NO KEY UPDATE";
									break;
								case LCS_FORUPDATE:
									tag = "SELECT FOR UPDATE";
									break;
								default:
									tag = "???";
									break;
							}
						}
						else
							tag = "SELECT";
						break;
					case CMD_UPDATE:
						tag = "UPDATE";
						break;
					case CMD_INSERT:
						tag = "INSERT";
						break;
					case CMD_DELETE:
						tag = "DELETE";
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(stmt->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = "???";
						break;
				}
			}
			break;

#ifdef ADB
		case T_ExecDirectStmt:
			tag = "EXECUTE DIRECT";
			break;
		case T_CleanConnStmt:
			tag = "CLEAN CONNECTION";
			break;
#endif

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			tag = "???";
			break;
	}

	return tag;
}


/*
 * GetCommandLogLevel
 *		utility to get the minimum log_statement level for a command,
 *		given either a raw (un-analyzed) parsetree or a planned query.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 */
LogStmtLevel
GetCommandLogLevel(Node *parsetree)
{
	LogStmtLevel lev;

	switch (nodeTag(parsetree))
	{
			/* raw plannable queries */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_SelectStmt:
			if (((SelectStmt *) parsetree)->intoClause)
				lev = LOGSTMT_DDL;		/* SELECT INTO */
			else
				lev = LOGSTMT_ALL;
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DeclareCursorStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClosePortalStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_FetchStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStmt:
		case T_CreateForeignTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
		case T_AlterTableSpaceOptionsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_AlterUserMappingStmt:
		case T_DropUserMappingStmt:
		case T_ImportForeignSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_TruncateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_CommentStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_SecLabelStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CopyStmt:
			if (((CopyStmt *) parsetree)->is_from)
				lev = LOGSTMT_MOD;
			else
				lev = LOGSTMT_ALL;
			break;

		case T_PrepareStmt:
			{
				PrepareStmt *stmt = (PrepareStmt *) parsetree;

				/* Look through a PREPARE to the contained stmt */
				lev = GetCommandLogLevel(stmt->query);
			}
			break;

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *ps;

				/* Look through an EXECUTE to the referenced stmt */
				ps = FetchPreparedStatement(stmt->name, false);
				if (ps && ps->plansource->raw_parse_tree)
					lev = GetCommandLogLevel(ps->plansource->raw_parse_tree);
				else
					lev = LOGSTMT_ALL;
			}
			break;

		case T_DeallocateStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_RenameStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectDependsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOwnerStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTableMoveAllStmt:
		case T_AlterTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDefaultPrivilegesStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DefineStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CompositeTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRangeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_IndexStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RuleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DoStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreatedbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropdbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_NotifyStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ListenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_UnlistenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_LoadStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClusterStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VacuumStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *stmt = (ExplainStmt *) parsetree;
				bool		analyze = false;
				ListCell   *lc;

				/* Look through an EXPLAIN ANALYZE to the contained stmt */
				foreach(lc, stmt->options)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if (strcmp(opt->defname, "analyze") == 0)
						analyze = defGetBoolean(opt);
					/* don't "break", as explain.c will use the last value */
				}
				if (analyze)
					return GetCommandLogLevel(stmt->query);

				/* Plain EXPLAIN isn't so interesting */
				lev = LOGSTMT_ALL;
			}
			break;

		case T_CreateTableAsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RefreshMatViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSystemStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VariableSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_VariableShowStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DiscardStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEventTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEventTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePLangStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ReassignOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_LockStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ConstraintsSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CheckPointStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ReindexStmt:
			lev = LOGSTMT_ALL;	/* should this be DDL? */
			break;

		case T_CreateConversionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateCastStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpClassStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTransformStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePolicyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterPolicyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSDictionaryStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSConfigurationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateAmStmt:
			lev = LOGSTMT_DDL;
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
						lev = LOGSTMT_MOD;
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

			/* parsed-and-rewritten-but-not-planned queries */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}

			}
			break;

#ifdef ADB
		case T_CreateNodeStmt:
		case T_AlterNodeStmt:
		case T_DropNodeStmt:
		case T_CreateGroupStmt:
		case T_DropGroupStmt:
		case T_CleanConnStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ExecDirectStmt:
		case T_BarrierStmt:
			lev = LOGSTMT_ALL;
			break;
#endif

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			lev = LOGSTMT_ALL;
			break;
	}

	return lev;
}

#ifdef ADB
/*
 * GetCommentObjectId
 * TODO Change to return the nodes to execute the utility on
 *
 * Return Object ID of object commented
 * Note: This function uses portions of the code of CommentObject,
 * even if this code is duplicated this is done like this to facilitate
 * merges with PostgreSQL head.
 */
static RemoteQueryExecType
GetNodesForCommentUtility(CommentStmt *stmt, bool *is_temp)
{
	ObjectAddress		address;
	Relation			relation;
	RemoteQueryExecType	exec_type = EXEC_ON_ALL_NODES;	/* By default execute on all nodes */
	Oid					object_id;

	if (stmt->objtype == OBJECT_DATABASE && list_length(stmt->objname) == 1)
	{
		char	   *database = strVal(linitial(stmt->objname));
		if (!OidIsValid(get_database_oid(database, true)))
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", database)));
		/* No clue, return the default one */
		return exec_type;
	}

	address = get_object_address(stmt->objtype, stmt->objname, stmt->objargs,
								 &relation, ShareUpdateExclusiveLock, false);
	object_id = address.objectId;

	/*
	 * If the object being commented is a rule, the nodes are decided by the
	 * object to which rule is applicable, so get the that object's oid
	 */
	if (stmt->objtype == OBJECT_RULE)
	{
		if (!relation && !OidIsValid(relation->rd_id))
		{
			/* This should not happen, but prepare for the worst */
			char *rulename = strVal(llast(stmt->objname));
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("can not find relation for rule \"%s\" does not exist", rulename)));
			object_id = InvalidOid;
		}
		else
			object_id = RelationGetRelid(relation);
	}

	if (relation != NULL)
		relation_close(relation, NoLock);

	/* Commented object may not have a valid object ID, so move to default */
	if (OidIsValid(object_id))
		exec_type = ExecUtilityFindNodes(stmt->objtype,
										 object_id,
										 is_temp);
	return exec_type;
}

/*
 * GetNodesForRulesUtility
 * Get the nodes to execute this RULE related utility statement.
 * A rule is expanded on Coordinator itself, and does not need any
 * existence on Datanode. In fact, if it were to exist on Datanode,
 * there is a possibility that it would expand again
 */
static RemoteQueryExecType
GetNodesForRulesUtility(RangeVar *relation, bool *is_temp)
{
	Oid relid = RangeVarGetRelid(relation, NoLock, true);
	RemoteQueryExecType exec_type;

	/* Skip if this Oid does not exist */
	if (!OidIsValid(relid))
		return EXEC_ON_NONE;

	/*
	 * PGXCTODO: See if it's a temporary object, do we really need
	 * to care about temporary objects here? What about the
	 * temporary objects defined inside the rule?
	 */
	exec_type = ExecUtilityFindNodes(OBJECT_RULE, relid, is_temp);
	return exec_type;
}

/*
 * TreatDropStmtOnCoord
 * Do a pre-treatment of Drop statement on a remote Coordinator
 */
/*
 * By utility.c refactoring to support event trigger, it is difficult fo callers to
 * supply queryString, which is not used in this function.
 */
static void
DropStmtPreTreatment(DropStmt *stmt, const char *queryString, bool sentToRemote,
					 bool *is_temp, RemoteQueryExecType *exec_type)
{
	bool		res_is_temp = false;
	RemoteQueryExecType res_exec_type = EXEC_ON_ALL_NODES;

	/* Nothing to do if not local Coordinator */
	if (!IsCoordMaster())
		return;

	switch (stmt->removeType)
	{
		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_INDEX:
			{
				/*
				 * Check the list of objects going to be dropped.
				 * XC does not allow yet to mix drop of temporary and
				 * non-temporary objects because this involves to rewrite
				 * query to process for tables.
				 */
				ListCell   *cell;
				bool		is_first = true;

				foreach(cell, stmt->objects)
				{
					RangeVar   *rel = makeRangeVarFromNameList((List *) lfirst(cell));
					Oid         relid;

					/*
					 * Do not print result at all, error is thrown
					 * after if necessary
					 */
					relid = RangeVarGetRelid(rel, NoLock, true);

					/*
					 * In case this relation ID is incorrect throw
					 * a correct DROP error.
					 */
					if (!OidIsValid(relid) && !stmt->missing_ok)
						DropTableThrowErrorExternal(rel,
													stmt->removeType,
													stmt->missing_ok);

					/* In case of DROP ... IF EXISTS bypass */
					if (!OidIsValid(relid) && stmt->missing_ok)
						continue;

					if (is_first)
					{
						res_exec_type = ExecUtilityFindNodes(stmt->removeType,
														 relid,
														 &res_is_temp);
						is_first = false;
					}
					else
					{
						RemoteQueryExecType exec_type_loc;
						bool is_temp_loc;
						exec_type_loc = ExecUtilityFindNodes(stmt->removeType,
															 relid,
															 &is_temp_loc);
						if (exec_type_loc != res_exec_type ||
							is_temp_loc != res_is_temp)
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("DROP not supported for TEMP and non-TEMP objects"),
									 errdetail("You should separate TEMP and non-TEMP objects")));
					}
				}
			}
			break;

		/*
		 * Those objects are dropped depending on the nature of the relationss
		 * they are defined on. This evaluation uses the temporary behavior
		 * and the relkind of the relation used.
		 */
		case OBJECT_RULE:
		case OBJECT_TRIGGER:
			{
				List *objname = linitial(stmt->objects);
				Relation    relation = NULL;

				get_object_address(stmt->removeType,
								   objname, NIL,
								   &relation,
								   AccessExclusiveLock,
								   stmt->missing_ok);

				/* Do nothing if no relation */
				if (relation && OidIsValid(relation->rd_id))
					res_exec_type = ExecUtilityFindNodes(stmt->removeType,
														 relation->rd_id,
														 &res_is_temp);
				else
					res_exec_type = EXEC_ON_NONE;

				/* Close relation if necessary */
				if (relation)
					relation_close(relation, NoLock);
			}
			break;

		default:
			res_is_temp = false;
			res_exec_type = *exec_type;
			break;
	}

	/* Save results */
	*is_temp = res_is_temp;
	*exec_type = res_exec_type;
}
#endif
