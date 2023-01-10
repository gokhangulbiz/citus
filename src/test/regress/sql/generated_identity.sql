CREATE SCHEMA generated_identities;
SET search_path TO generated_identities;
SET client_min_messages to ERROR;

select 1 from citus_add_node('localhost', :master_port, groupId=>0);

DROP TABLE IF EXISTS generated_identities_test;

-- create a partitioned table for testing.
CREATE TABLE generated_identities_test (
    a int GENERATED BY DEFAULT AS IDENTITY,
    b bigint GENERATED ALWAYS AS IDENTITY (START WITH 10 INCREMENT BY 10),
    c smallint GENERATED BY DEFAULT AS IDENTITY,
    d serial,
    e bigserial,
    f smallserial,
    g int
)
PARTITION BY RANGE (a);
CREATE TABLE generated_identities_test_1_5 PARTITION OF generated_identities_test FOR VALUES FROM (1) TO (5);
CREATE TABLE generated_identities_test_5_50 PARTITION OF generated_identities_test FOR VALUES FROM (5) TO (50);

-- local tables
SELECT citus_add_local_table_to_metadata('generated_identities_test');

\d generated_identities_test

\c - - - :worker_1_port

\d generated_identities.generated_identities_test

\c - - - :master_port
SET search_path TO generated_identities;
SET client_min_messages to ERROR;

select undistribute_table('generated_identities_test');

SELECT citus_remove_node('localhost', :master_port);

select create_distributed_table('generated_identities_test', 'a');

\d generated_identities_test

\c - - - :worker_1_port

\d generated_identities.generated_identities_test

\c - - - :master_port
SET search_path TO generated_identities;
SET client_min_messages to ERROR;

insert into generated_identities_test (g) values (1);

insert into generated_identities_test (g) select 2;

INSERT INTO generated_identities_test (g)
SELECT s FROM generate_series(3,7) s;

SELECT * FROM generated_identities_test ORDER BY 1;

select undistribute_table('generated_identities_test');

SELECT * FROM generated_identities_test ORDER BY 1;

\d generated_identities_test

\c - - - :worker_1_port

\d generated_identities.generated_identities_test

\c - - - :master_port
SET search_path TO generated_identities;
SET client_min_messages to ERROR;

INSERT INTO generated_identities_test (g)
SELECT s FROM generate_series(8,10) s;

SELECT * FROM generated_identities_test ORDER BY 1;

-- distributed table
select create_distributed_table('generated_identities_test', 'a');

-- alter table is unsupported
ALTER TABLE generated_identities_test ALTER COLUMN g ADD GENERATED ALWAYS AS IDENTITY;

select alter_distributed_table('generated_identities_test', 'g');

select alter_distributed_table('generated_identities_test', 'b');

select alter_distributed_table('generated_identities_test', 'c');

select undistribute_table('generated_identities_test');

SELECT * FROM generated_identities_test ORDER BY g;

-- reference table

DROP TABLE generated_identities_test;

CREATE TABLE generated_identities_test (
    a int GENERATED BY DEFAULT AS IDENTITY,
    b bigint GENERATED ALWAYS AS IDENTITY (START WITH 10 INCREMENT BY 10),
    c smallint GENERATED BY DEFAULT AS IDENTITY,
    d serial,
    e bigserial,
    f smallserial,
    g int
);

SELECT create_reference_table('generated_identities_test');

\d generated_identities_test

\c - - - :worker_1_port

\d generated_identities.generated_identities_test

\c - - - :master_port
SET search_path TO generated_identities;
SET client_min_messages to ERROR;

INSERT INTO generated_identities_test (g)
SELECT s FROM generate_series(11,20) s;

SELECT * FROM generated_identities_test ORDER BY g;

select undistribute_table('generated_identities_test');

\d generated_identities_test

\c - - - :worker_1_port

\d generated_identities.generated_identities_test

\c - - - :master_port
SET search_path TO generated_identities;
SET client_min_messages to ERROR;

DROP SCHEMA generated_identities CASCADE;
