\set ON_ERROR_STOP on

CREATE EXTENSION bloompg;

CREATE UNLOGGED TABLE demo_customers AS
SELECT id, 'region-' || (id % 10)::text AS region
FROM generate_series(1, 100000) AS id;

CREATE UNLOGGED TABLE demo_products AS
SELECT id, 'category-' || (id % 10)::text AS category
FROM generate_series(1, 10000) AS id;

CREATE UNLOGGED TABLE demo_stores AS
SELECT id, 'country-' || (id % 10)::text AS country
FROM generate_series(1, 1000) AS id;

CREATE UNLOGGED TABLE demo_campaigns AS
SELECT id, 'channel-' || (id % 10)::text AS channel
FROM generate_series(1, 1000) AS id;

CREATE UNLOGGED TABLE demo_sales AS
SELECT
    id,
    1 + (id % 100000) AS customer_id,
    1 + ((id / 97) % 10000) AS product_id,
    1 + ((id / 7919) % 1000) AS store_id,
    1 + ((id / 104729) % 1000) AS campaign_id,
    repeat('analytical sale row ', 4) ||
        CASE WHEN id % 2 = 0 THEN 'keep' ELSE 'drop' END AS payload
FROM generate_series(1, 5000000) AS id;

ANALYZE demo_customers;
ANALYZE demo_products;
ANALYZE demo_stores;
ANALYZE demo_campaigns;
ANALYZE demo_sales;
