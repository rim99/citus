/*
 * worker_shard_visibility.c
 *
 * Implements the functions for hiding shards on the Citus MX
 * worker (data) nodes. This is mostly required for
 *
 *  * Copyright (c) 2018, Citus Data, Inc.
 */

#include "postgres.h"

#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "distributed/metadata_cache.h"
#include "distributed/worker_protocol.h"
#include "distributed/worker_shard_visibility.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* Config variable managed via guc.c */
bool EnableReplaceTableVisibleFunction = true;

static bool RelationIsAKnownShard(Oid shardRelationId);
static char * citus_strndup(const char *s, size_t n);
static Node * ReplaceTableVisibleFunctionWalker(Node *inputNode);

PG_FUNCTION_INFO_V1(citus_table_is_visible);
PG_FUNCTION_INFO_V1(relation_is_a_known_shard);


/*
 * relation_is_a_known_shard a wrapper around RelationIsAKnownShard(), so
 * see the details there. The function also treats the indexes on shards
 * as if they were shards.
 */
Datum
relation_is_a_known_shard(PG_FUNCTION_ARGS)
{
	Oid relationId = PG_GETARG_OID(0);

	CheckCitusVersion(ERROR);

	PG_RETURN_BOOL(RelationIsAKnownShard(relationId));
}


/*
 * citus_table_is_visible aims to behave exactly the same with
 * pg_table_is_visible with only one exception. The former one
 * returns false for the relations that are known to be shards.
 */
Datum
citus_table_is_visible(PG_FUNCTION_ARGS)
{
	Oid relationId = PG_GETARG_OID(0);
	char relKind = '\0';

	CheckCitusVersion(ERROR);

	/*
	 * We don't want to deal with not valid/existing relations
	 * as pg_table_is_visible does.
	 */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relationId)))
	{
		PG_RETURN_NULL();
	}

	if (RelationIsAKnownShard(relationId))
	{
		/*
		 * If the input relation is an index we simply replace the
		 * relationId with the corresponding relation to hide indexes
		 * as well. See RelationIsAKnownShard() for the details and give
		 * more meaningful debug message here.
		 */
		relKind = get_rel_relkind(relationId);
		if (relKind == RELKIND_INDEX)
		{
			ereport(DEBUG1, (errmsg("skipping index \"%s\" since it belongs to a shard",
									get_rel_name(relationId))));
		}
		else
		{
			ereport(DEBUG1, (errmsg("skipping relation \"%s\" since it is a shard",
									get_rel_name(relationId))));
		}

		return BoolGetDatum(false);
	}

	return BoolGetDatum(RelationIsVisible(relationId));
}


/*
 * RelationIsAKnownShard gets a relationId, check whether it's a shard of
 * any distributed table in the current search path.
 *
 * We can only do that in MX since both the metadata and tables are only
 * present there.
 */
static bool
RelationIsAKnownShard(Oid shardRelationId)
{
	int localGroupId = -1;
	char *shardRelationName = NULL;
	char *relationName = NULL;
	bool missingOk = true;
	uint64 shardId = INVALID_SHARD_ID;
	Oid relationId = InvalidOid;
	char *shardIdString = NULL;
	int relationNameLength = 0;
	char relKind = '\0';

	List *shardIdList = NIL;
	ListCell *shardIdCell = NULL;

	if (!OidIsValid(shardRelationId))
	{
		/* we cannot continue without a valid Oid */
		return false;
	}

	localGroupId = GetLocalGroupId();
	if (localGroupId == 0)
	{
		/*
		 * We're not interested in shards in the coordinator
		 * or non-mx worker nodes.
		 */
		return false;
	}

	/* we're not interested in the relations that are not in the search path */
	if (!RelationIsVisible(shardRelationId))
	{
		return false;
	}

	/*
	 * If the input relation is an index we simply replace the
	 * relationId with the corresponding relation to hide indexes
	 * as well.
	 */
	relKind = get_rel_relkind(shardRelationId);
	if (relKind == RELKIND_INDEX)
	{
		shardRelationId = IndexGetRelation(shardRelationId, false);
	}

	/* get the shard's relation name */
	shardRelationName = get_rel_name(shardRelationId);

	/* find the last underscore for shardId string */
	shardIdString = strrchr(shardRelationName, SHARD_NAME_SEPARATOR);
	if (shardIdString == NULL)
	{
		/* there are no underscore in the table name */
		return false;
	}

	relationNameLength = shardIdString - shardRelationName;
	relationName = citus_strndup(shardRelationName, relationNameLength);

	relationId = RelnameGetRelid(relationName);
	if (!OidIsValid(relationId))
	{
		/* there is no such relation */
		return false;
	}

	if (!IsDistributedTable(relationId))
	{
		/* we're  obviously only interested in distributed tables */
		return false;
	}

	shardId = ExtractShardId(shardRelationName, missingOk);
	if (shardId == INVALID_SHARD_ID)
	{
		/*
		 * The format of the table name does not align with
		 * our shard name definition.
		 */
		return false;
	}

	/*
	 * Finally make sure that the input shard belongs to
	 * the distributed table we've found.
	 */
	shardIdList = LoadShardList(relationId);
	foreach(shardIdCell, shardIdList)
	{
		uint64 *shardIdPointer = (uint64 *) lfirst(shardIdCell);
		uint64 currentShardId = (*shardIdPointer);

		if (shardId == currentShardId)
		{
			return true;
		}
	}

	return false;
}


/*
 * strndup is not part of Standard C and extension of Posix.1-2008.
 * So, implement our own strndup to make it work on Windows.
 */
static char *
citus_strndup(const char *s, size_t n)
{
	char *result;
	size_t len = strlen(s);

	if (n < len)
	{
		len = n;
	}

	result = (char *) malloc(len + 1);
	if (!result)
	{
		return NULL;
	}

	result[len] = '\0';
	return (char *) memcpy(result, s, len);
}


/*
 * ReplaceTableVisibleFunction is a wrapper around ReplaceTableVisibleFunctionWalker.
 * The replace functionality can be enabled/disable via a GUC. This function also
 * ensures that the extension is loaded and the version is compatible.
 */
Node *
ReplaceTableVisibleFunction(Node *inputNode)
{
	if (!EnableReplaceTableVisibleFunction ||
		!CitusHasBeenLoaded() || !CheckCitusVersion(DEBUG2))
	{
		return inputNode;
	}

	return ReplaceTableVisibleFunctionWalker(inputNode);
}


/*
 * ReplaceTableVisibleFunction replaces all occurences of
 * pg_catalog.pg_table_visible() to
 * pg_catalog.citus_table_visible() in the given input node.
 *
 * Note that the only difference between the functions is that
 * the latter filters the tables that are known to be shards on
 * Citus MX worker (data) nodes.
 */
static Node *
ReplaceTableVisibleFunctionWalker(Node *inputNode)
{
	if (inputNode == NULL)
	{
		return NULL;
	}

	if (IsA(inputNode, FuncExpr))
	{
		FuncExpr *functionToProcess = (FuncExpr *) inputNode;
		Oid functionId = functionToProcess->funcid;

		if (functionId == PgTableVisibleFuncId())
		{
			/*
			 * We simply update the function id of the FuncExpr for
			 * two reasons: (i) We don't want to interfere with the
			 * memory contexts so don't want to deal with allocating
			 * a new functionExpr (ii) We already know that both
			 * functions have the exact same signature.
			 */
			functionToProcess->funcid = CitusTableVisibleFuncId();

			return (Node *) functionToProcess;
		}
	}
	else if (IsA(inputNode, Query))
	{
		return (Node *) query_tree_mutator((Query *) inputNode,
										   ReplaceTableVisibleFunctionWalker, NULL, 0);
	}

	return expression_tree_mutator(inputNode, ReplaceTableVisibleFunctionWalker, NULL);
}
