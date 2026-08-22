\set ON_ERROR_STOP on

CREATE EXTENSION bloompg;

CREATE UNLOGGED TABLE demo_regions AS
SELECT id, 'region-' || id::text AS name
FROM generate_series(1, 100) AS id;

CREATE UNLOGGED TABLE demo_customers AS
SELECT id, 1 + (id % 100) AS region_id
FROM generate_series(1, 100000) AS id;

CREATE UNLOGGED TABLE demo_orders AS
SELECT id, 1 + (id % 100000) AS customer_id
FROM generate_series(1, 1000000) AS id;

CREATE UNLOGGED TABLE demo_lineitems AS
SELECT id, 1 + (id % 1000000) AS order_id
FROM generate_series(1, 3000000) AS id;

ANALYZE demo_regions;
ANALYZE demo_customers;
ANALYZE demo_orders;
ANALYZE demo_lineitems;

