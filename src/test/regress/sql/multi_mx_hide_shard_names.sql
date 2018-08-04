--
-- Hide shard names on MX worker nodes
--

SET citus.next_shard_id TO 1130000;


CREATE SCHEMA mx_hide_shard_names;
SET search_path TO 'mx_hide_shard_names';

SET citus.shard_count TO 4;
SET citus.shard_replication_factor TO 1;

SET citus.replication_model TO 'streaming';
SELECT start_metadata_sync_to_node('localhost', :worker_1_port);
SELECT start_metadata_sync_to_node('localhost', :worker_2_port);

CREATE TABLE test_table(id int, time date);
SELECT create_distributed_table('test_table', 'id');

-- first show that the views does not show
-- any shards on the coordinator as expected
SELECT * FROM citus_shards_on_worker;
SELECT * FROM citus_shard_indexes_on_worker;

-- now show that we see the shards, but not the 
-- indexes as there are no indexes
\c - - - :worker_1_port
SET search_path TO 'mx_hide_shard_names';
SELECT * FROM citus_shards_on_worker ORDER BY 2;
SELECT * FROM citus_shard_indexes_on_worker ORDER BY 2;

-- now create an index
\c - - - :master_port
SET search_path TO 'mx_hide_shard_names';
CREATE INDEX test_index ON mx_hide_shard_names.test_table(id);

-- now show that we see the shards, and the 
-- indexes as well
\c - - - :worker_1_port
SET search_path TO 'mx_hide_shard_names';
SELECT * FROM citus_shards_on_worker ORDER BY 2;
SELECT * FROM citus_shard_indexes_on_worker ORDER BY 2;

-- we should be able to select from the shards directly if we 
-- know the name of the tables
SELECT count(*) FROM test_table_1130000;

-- disable the config so that table becomes visible
SELECT pg_table_is_visible('test_table_1130000'::regclass);
SET citus.enable_replace_table_visible_function TO FALSE;
SELECT pg_table_is_visible('test_table_1130000'::regclass);

\c - - - :master_port
-- make sure that we're resilient to the edge cases
-- such that the table name includes the shard number
SET search_path TO 'mx_hide_shard_names';
SET citus.shard_count TO 4;
SET citus.shard_replication_factor TO 1;

SET citus.replication_model TO 'streaming';
CREATE TABLE test_table_102008(id int, time date);
SELECT create_distributed_table('test_table_102008', 'id');

\c - - - :worker_1_port
SET search_path TO 'mx_hide_shard_names';
SELECT * FROM citus_shards_on_worker ORDER BY 2;


\c - - - :master_port
-- make sure that don't mess up with schemas
CREATE SCHEMA mx_hide_shard_names_2;
SET search_path TO 'mx_hide_shard_names_2';
SET citus.shard_count TO 4;
SET citus.shard_replication_factor TO 1;

SET citus.replication_model TO 'streaming';
CREATE TABLE test_table(id int, time date);
SELECT create_distributed_table('test_table', 'id');
CREATE INDEX test_index ON mx_hide_shard_names_2.test_table(id);

\c - - - :worker_1_port
SET search_path TO 'mx_hide_shard_names';
SELECT * FROM citus_shards_on_worker ORDER BY 2;
SELECT * FROM citus_shard_indexes_on_worker ORDER BY 2;
SET search_path TO 'mx_hide_shard_names_2';
SELECT * FROM citus_shards_on_worker ORDER BY 2;
SELECT * FROM citus_shard_indexes_on_worker ORDER BY 2;
SET search_path TO 'mx_hide_shard_names_2, mx_hide_shard_names';
SELECT * FROM citus_shards_on_worker ORDER BY 2;
SELECT * FROM citus_shard_indexes_on_worker ORDER BY 2;

-- clean-up
\c - - - :master_port


-- show that common psql functions do not show shards
-- including the ones that are not in the current schema
SET search_path TO 'mx_hide_shard_names';
\d
\di

DROP SCHEMA mx_hide_shard_names CASCADE;
DROP SCHEMA mx_hide_shard_names_2 CASCADE;
