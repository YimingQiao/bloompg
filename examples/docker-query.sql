\set ON_ERROR_STOP on
\timing on

SELECT bloompg_version();
SET bloompg.sample_mode = 'instant';

\echo ''
\echo 'Native PostgreSQL:'
SET bloompg.enable = off;
SELECT count(*) AS matching_sales
FROM demo_sales AS s
JOIN demo_customers AS c ON c.id = s.customer_id
JOIN demo_products AS p ON p.id = s.product_id
JOIN demo_stores AS st ON st.id = s.store_id
JOIN demo_campaigns AS ca ON ca.id = s.campaign_id
WHERE c.region = 'region-7'
  AND p.category = 'category-7'
  AND st.country = 'country-7'
  AND ca.channel = 'channel-7'
  AND lower(s.payload) LIKE '%keep%';

\echo ''
\echo 'BloomPG:'
SET bloompg.enable = on;
SET bloompg.profile = on;
SET bloompg.profile_log = off;
SELECT count(*) AS matching_sales
FROM demo_sales AS s
JOIN demo_customers AS c ON c.id = s.customer_id
JOIN demo_products AS p ON p.id = s.product_id
JOIN demo_stores AS st ON st.id = s.store_id
JOIN demo_campaigns AS ca ON ca.id = s.campaign_id
WHERE c.region = 'region-7'
  AND p.category = 'category-7'
  AND st.country = 'country-7'
  AND ca.channel = 'channel-7'
  AND lower(s.payload) LIKE '%keep%';

\echo ''
\echo 'Latest BloomPG transfer trace:'
SELECT bloompg_last_trace();
