/*-------------------------------------------------------------------------
 *
 * coordinator_protocol.h
 *	  Header for shared declarations for access to coordinator node data.
 *    These data are used to create new shards or update existing ones.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COORDINATOR_PROTOCOL_H
#define COORDINATOR_PROTOCOL_H

#include "postgres.h"
#include "c.h"
#include "fmgr.h"

#include "distributed/connection_management.h"
#include "distributed/shardinterval_utils.h"
#include "nodes/pg_list.h"
#include "distributed/metadata_utility.h"

#include "columnar/columnar.h"

/*
 * In our distributed database, we need a mechanism to make remote procedure
 * calls between clients, the coordinator node, and worker nodes. These remote calls
 * require serializing and deserializing values and function signatures between
 * nodes; and for these, we currently use PostgreSQL's built-in type and
 * function definition system. This approach is by no means ideal however; and
 * our implementation: (i) cannot perform compile-time type checks, (ii)
 * requires additional effort when upgrading to new function signatures, and
 * (iii) hides argument and return value names and types behind complicated
 * pg_proc.h definitions.
 *
 * An ideal implementation should overcome these problems, and make it much
 * easier to pass values back and forth between nodes. One such implementation
 * that comes close to ideal is Google's Protocol Buffers. Nonetheless, we do
 * not use it in here as its inclusion requires changes to PostgreSQL's make
 * system, and a native C version is currently unavailable.
 */


/* Number of tuple fields that coordinator node functions return */
#define TABLE_METADATA_FIELDS 7
#define CANDIDATE_NODE_FIELDS 2
#define WORKER_NODE_FIELDS 2

/* transfer mode for citus_copy_shard_placement */
#define TRANSFER_MODE_AUTOMATIC 'a'
#define TRANSFER_MODE_FORCE_LOGICAL 'l'
#define TRANSFER_MODE_BLOCK_WRITES 'b'

#define SHARDID_SEQUENCE_NAME "pg_dist_shardid_seq"
#define PLACEMENTID_SEQUENCE_NAME "pg_dist_placement_placementid_seq"

/* Remote call definitions to help with data staging and deletion */
#define WORKER_APPLY_SHARD_DDL_COMMAND \
	"SELECT worker_apply_shard_ddl_command (" UINT64_FORMAT ", %s, %s)"
#define WORKER_APPLY_SHARD_DDL_COMMAND_WITHOUT_SCHEMA \
	"SELECT worker_apply_shard_ddl_command (" UINT64_FORMAT ", %s)"
#define WORKER_APPLY_INTER_SHARD_DDL_COMMAND \
	"SELECT worker_apply_inter_shard_ddl_command (" UINT64_FORMAT ", %s, " UINT64_FORMAT \
	", %s, %s)"
#define SHARD_RANGE_QUERY "SELECT min(%s), max(%s) FROM %s"
#define SHARD_TABLE_SIZE_QUERY "SELECT pg_table_size(%s)"
#define SHARD_CSTORE_TABLE_SIZE_QUERY "SELECT cstore_table_size(%s)"
#define DROP_REGULAR_TABLE_COMMAND "DROP TABLE IF EXISTS %s CASCADE"
#define DROP_FOREIGN_TABLE_COMMAND "DROP FOREIGN TABLE IF EXISTS %s CASCADE"
#define CREATE_SCHEMA_COMMAND "CREATE SCHEMA IF NOT EXISTS %s AUTHORIZATION %s"

/*
 * TableDDLCommandType encodes the implementation used by TableDDLCommand. See comments in
 * TableDDLCpmmand for details.
 */
typedef enum TableDDLCommandType
{
	TABLE_DDL_COMMAND_STRING,
	TABLE_DDL_COMMAND_FUNCTION,
} TableDDLCommandType;


/*
 * IndexDefinitionDeparseFlags helps to control which parts of the
 * index creation commands are deparsed.
 */
typedef enum IndexDefinitionDeparseFlags
{
	INCLUDE_CREATE_INDEX_STATEMENTS = 1 << 0,
	INCLUDE_CREATE_CONSTRAINT_STATEMENTS = 1 << 1,
	INCLUDE_INDEX_CLUSTERED_STATEMENTS = 1 << 2,
	INCLUDE_INDEX_STATISTICS_STATEMENTTS = 1 << 3,
	INCLUDE_INDEX_ALL_STATEMENTS = INCLUDE_CREATE_INDEX_STATEMENTS |
								   INCLUDE_CREATE_CONSTRAINT_STATEMENTS |
								   INCLUDE_INDEX_CLUSTERED_STATEMENTS |
								   INCLUDE_INDEX_STATISTICS_STATEMENTTS
} IndexDefinitionDeparseFlags;


/*
 * IncludeSequenceDefaults decides on inclusion of DEFAULT clauses for columns
 * getting their default values from a sequence when creating the definition
 * of a table.
 */
typedef enum IncludeSequenceDefaults
{
	NO_SEQUENCE_DEFAULTS = 0, /* don't include sequence defaults */
	NEXTVAL_SEQUENCE_DEFAULTS = 1, /* include sequence defaults */

	/*
	 * Include sequence defaults, but use worker_nextval instead of nextval
	 * when the default will be called in worker node, and the column type is
	 * int or smallint.
	 */
	WORKER_NEXTVAL_SEQUENCE_DEFAULTS = 2,
} IncludeSequenceDefaults;


/*
 * IncludeIdentities decides on how we include identity information
 * when creating the definition of a table.
 */
typedef enum IncludeIdentities
{
	NO_IDENTITY = 0, /* don't include identities */
	INCLUDE_IDENTITY = 1 /* include identities as-is*/
} IncludeIdentities;


struct TableDDLCommand;
typedef struct TableDDLCommand TableDDLCommand;
typedef char *(*TableDDLFunction)(void *context);
typedef char *(*TableDDLShardedFunction)(uint64 shardId, void *context);

/*
 * TableDDLCommand holds the definition of a command to be executed to bring the table and
 * or shard into a certain state. The command needs to be able to serialized into two
 * versions:
 *  - one version should have the vanilla commands operating on the base table. These are
 *    used for example to create the MX table shards
 *  - the second versions should replace all identifiers with an identifier containing the
 *    shard id.
 *
 * Current implementations are
 *  - command string, created via makeTableDDLCommandString. This variant contains a ddl
 *    command that will be wrapped in `worker_apply_shard_ddl_command` when applied
 *    against a shard.
 */
struct TableDDLCommand
{
	CitusNode node;

	/* encoding the type this TableDDLCommand contains */
	TableDDLCommandType type;

	/*
	 * This union contains one (1) typed field for every implementation for
	 * TableDDLCommand. A union enforces no overloading of fields but instead requires at
	 * most one of the fields to be used at any time.
	 */
	union
	{
		/*
		 * CommandStr is used when type is set to TABLE_DDL_COMMAND_STRING. It holds the
		 * sql ddl command string representing the ddl command.
		 */
		char *commandStr;

		/*
		 * function is used when type is set to TABLE_DDL_COMMAND_FUNCTION. It contains
		 * function pointers and a context to be passed to the functions to be able to
		 * construct the sql commands for sharded and non-sharded tables.
		 */
		struct
		{
			TableDDLFunction function;
			TableDDLShardedFunction shardedFunction;
			void *context;
		}
		function;
	};
};

/*
 * ColumnarTableDDLContext holds the instance variable for the TableDDLCommandFunction
 * instance described below.
 */
typedef struct ColumnarTableDDLContext
{
	char *schemaName;
	char *relationName;
	ColumnarOptions options;
} ColumnarTableDDLContext;

/* make functions for TableDDLCommand */
extern TableDDLCommand * makeTableDDLCommandString(char *commandStr);
extern TableDDLCommand * makeTableDDLCommandFunction(TableDDLFunction function,
													 TableDDLShardedFunction
													 shardedFunction,
													 void *context);

extern char * GetShardedTableDDLCommand(TableDDLCommand *command, uint64 shardId,
										char *schemaName);
extern char * GetShardedTableDDLCommandColumnar(uint64 shardId, void *context);
extern char * GetTableDDLCommand(TableDDLCommand *command);
extern TableDDLCommand * ColumnarGetCustomTableOptionsDDL(char *schemaName,
														  char *relationName,
														  ColumnarOptions options);


/* Config variables managed via guc.c */
extern int ShardCount;
extern int ShardReplicationFactor;
extern int NextShardId;
extern int NextPlacementId;


extern bool IsCoordinator(void);

/* Function declarations local to the distributed module */
extern uint64 GetNextShardId(void);
extern uint64 GetNextPlacementId(void);
extern Oid ResolveRelationId(text *relationName, bool missingOk);
extern List * GetFullTableCreationCommands(Oid relationId,
										   IncludeSequenceDefaults includeSequenceDefaults,
										   IncludeIdentities includeIdentityDefaults,
										   bool creatingShellTableOnRemoteNode);
extern List * GetPostLoadTableCreationCommands(Oid relationId, bool includeIndexes,
											   bool includeReplicaIdentity);
extern List * GetPreLoadTableCreationCommands(Oid relationId, IncludeSequenceDefaults
											  includeSequenceDefaults,
											  IncludeIdentities
											  includeIdentityDefaults,
											  char *accessMethod);
extern List * GetTableRowLevelSecurityCommands(Oid relationId);
extern List * GetTableIndexAndConstraintCommands(Oid relationId, int indexFlags);
extern List * GetTableIndexAndConstraintCommandsExcludingReplicaIdentity(Oid relationId,
																		 int indexFlags);
extern Oid GetRelationIdentityOrPK(Relation rel);
extern void GatherIndexAndConstraintDefinitionList(Form_pg_index indexForm,
												   List **indexDDLEventList,
												   int indexFlags);
extern bool IndexImpliedByAConstraint(Form_pg_index indexForm);
extern List * GetTableReplicaIdentityCommand(Oid relationId);
extern char ShardStorageType(Oid relationId);
extern bool DistributedTableReplicationIsEnabled(void);
extern void CheckDistributedTable(Oid relationId);
extern void CreateAppendDistributedShardPlacements(Oid relationId, int64 shardId,
												   List *workerNodeList, int
												   replicationFactor);
extern void CreateShardsOnWorkers(Oid distributedRelationId, List *shardPlacements,
								  bool useExclusiveConnection);
extern void InsertShardPlacementRows(Oid relationId, int64 shardId,
									 List *workerNodeList, int workerStartIndex,
									 int replicationFactor);
extern uint64 UpdateShardStatistics(int64 shardId);
extern void CreateShardsWithRoundRobinPolicy(Oid distributedTableId, int32 shardCount,
											 int32 replicationFactor,
											 bool useExclusiveConnections);
extern void CreateColocatedShards(Oid targetRelationId, Oid sourceRelationId,
								  bool useExclusiveConnections);
extern void CreateReferenceTableShard(Oid distributedTableId);
extern void CreateSingleShardTableShardWithRoundRobinPolicy(Oid relationId,
															uint32 colocationId);
extern int EmptySingleShardTableColocationDecideNodeId(uint32 colocationId);
extern List * WorkerCreateShardCommandList(Oid relationId, uint64 shardId,
										   List *ddlCommandList);
extern Oid ForeignConstraintGetReferencedTableId(const char *queryString);
extern void CheckHashPartitionedTable(Oid distributedTableId);
extern void CheckTableSchemaNameForDrop(Oid relationId, char **schemaName,
										char **tableName);
extern text * IntegerToText(int32 value);

/* Function declarations for generating metadata for shard and placement creation */
extern Datum master_get_table_metadata(PG_FUNCTION_ARGS);
extern Datum master_get_table_ddl_events(PG_FUNCTION_ARGS);
extern Datum master_get_new_shardid(PG_FUNCTION_ARGS);
extern Datum master_get_new_placementid(PG_FUNCTION_ARGS);
extern Datum master_get_active_worker_nodes(PG_FUNCTION_ARGS);
extern Datum master_get_round_robin_candidate_nodes(PG_FUNCTION_ARGS);
extern Datum master_stage_shard_row(PG_FUNCTION_ARGS);
extern Datum master_stage_shard_placement_row(PG_FUNCTION_ARGS);

/* Function declarations to help with data staging and deletion */
extern Datum master_create_empty_shard(PG_FUNCTION_ARGS);
extern Datum master_update_shard_statistics(PG_FUNCTION_ARGS);
extern Datum master_drop_sequences(PG_FUNCTION_ARGS);
extern Datum master_modify_multiple_shards(PG_FUNCTION_ARGS);
extern Datum lock_relation_if_exists(PG_FUNCTION_ARGS);
extern Datum citus_drop_all_shards(PG_FUNCTION_ARGS);
extern Datum master_drop_all_shards(PG_FUNCTION_ARGS);
extern int MasterDropAllShards(Oid relationId, char *schemaName, char *relationName);

/* function declarations for shard creation functionality */
extern Datum master_create_worker_shards(PG_FUNCTION_ARGS);
extern Datum isolate_tenant_to_new_shard(PG_FUNCTION_ARGS);

/* function declarations for shard split functionality */
extern Datum citus_split_shard_by_split_points(PG_FUNCTION_ARGS);

/* function declarations for shard copy functinality */
extern List * CopyShardForeignConstraintCommandList(ShardInterval *shardInterval);
extern void CopyShardForeignConstraintCommandListGrouped(ShardInterval *shardInterval,
														 List **
														 colocatedShardForeignConstraintCommandList,
														 List **
														 referenceTableForeignConstraintList);
extern ShardPlacement * SearchShardPlacementInList(List *shardPlacementList,
												   const char *nodeName, uint32 nodePort);
extern ShardPlacement * SearchShardPlacementInListOrError(List *shardPlacementList,
														  const char *nodeName,
														  uint32 nodePort);
extern char LookupShardTransferMode(Oid shardReplicationModeOid);
extern void BlockWritesToShardList(List *shardList);
extern List * WorkerApplyShardDDLCommandList(List *ddlCommandList, int64 shardId);


#endif   /* COORDINATOR_PROTOCOL_H */
