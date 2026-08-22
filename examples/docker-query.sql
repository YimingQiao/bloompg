\set ON_ERROR_STOP on
\timing on

SELECT bloompg_version();

-- Keep the intentionally poor left-deep join order below. This models a
-- constrained or misestimated analytical plan whose selective relation is
-- reached last; BloomPG can propagate that selectivity across the whole graph.
SET join_collapse_limit = 1;

\echo ''
\echo 'Preparing BloomPG reusable samples (outside the comparison):'
\timing off
SET bloompg.enable = on;
\o /dev/null
SELECT count(*)
FROM demo_lineitems AS l
JOIN demo_orders AS o ON o.id = l.order_id
JOIN demo_customers AS c ON c.id = o.customer_id
JOIN demo_regions AS r ON r.id = c.region_id
WHERE r.name = 'region-7';
\o
\timing on

\echo ''
\echo 'Native PostgreSQL:'
SET bloompg.enable = off;
SELECT count(*) AS matching_lineitems
FROM demo_lineitems AS l
JOIN demo_orders AS o ON o.id = l.order_id
JOIN demo_customers AS c ON c.id = o.customer_id
JOIN demo_regions AS r ON r.id = c.region_id
WHERE r.name = 'region-7';

\echo ''
\echo 'BloomPG:'
SET bloompg.enable = on;
SET bloompg.profile = on;
SET bloompg.profile_log = off;
SELECT count(*) AS matching_lineitems
FROM demo_lineitems AS l
JOIN demo_orders AS o ON o.id = l.order_id
JOIN demo_customers AS c ON c.id = o.customer_id
JOIN demo_regions AS r ON r.id = c.region_id
WHERE r.name = 'region-7';

\echo ''
\echo 'Latest BloomPG transfer trace:'
SELECT bloompg_last_trace();
