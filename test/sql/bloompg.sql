CREATE EXTENSION bloompg;

SELECT bloompg_version() = 'bloompg 0.1.2 (PostgreSQL 18)' AS version_ok;
SELECT current_setting('bloompg.enable') = 'on' AS enabled_by_default;
SELECT current_setting('bloompg.sample_mode') = 'prepared' AS prepared_by_default;
SELECT current_setting('bloompg.transfer_progress_metric') = 'ndv'
    AS transfer_progress_metric_default;
SELECT current_setting('bloompg.sample_rate')::numeric = 0.01
    AS intermediate_sample_rate_default;
SELECT max_val::integer = 128 AS transfer_worker_limit_ok
FROM pg_settings
WHERE name = 'bloompg.transfer_workers';

CREATE SCHEMA bloompg_regress;
SET search_path = bloompg_regress, public;
SET max_parallel_workers_per_gather = 0;
SET bloompg.sample_mode = 'instant';
SET bloompg.late_materialize = on;
SET bloompg.late_materialize_min_width = '64B';

CREATE TABLE a(k integer, k2 integer, t text, d date, keep boolean, payload text);
CREATE TABLE b(k bigint, k2 integer, t text, d date, payload text);
CREATE TABLE c(k integer, k2 integer, flag boolean);

INSERT INTO a
SELECT CASE WHEN g % 97 = 0 THEN NULL ELSE g % 500 END,
       g % 17,
       't' || (g % 29),
       date '2020-01-01' + (g % 180),
       g < 25 OR g % 211 = 0,
       repeat('a', 160 + g % 40)
FROM generate_series(1, 2000) AS g;

INSERT INTO b
SELECT CASE WHEN g % 89 = 0 THEN NULL ELSE g % 650 END,
       g % 17,
       't' || (g % 31),
       date '2020-01-01' + (g % 190),
       repeat('b', 140 + g % 30)
FROM generate_series(1, 3000) AS g;

INSERT INTO c
SELECT CASE WHEN g % 101 = 0 THEN NULL ELSE g % 550 END,
       g % 17,
       g % 13 = 0
FROM generate_series(1, 2500) AS g;

CREATE INDEX ON a(k);
CREATE INDEX ON b(k);
CREATE INDEX ON c(k);
ANALYZE;

CREATE TABLE pa(k integer, keep boolean) PARTITION BY RANGE (k);
CREATE TABLE pa0 PARTITION OF pa FOR VALUES FROM (0) TO (250);
CREATE TABLE pa1 PARTITION OF pa FOR VALUES FROM (250) TO (500);
INSERT INTO pa SELECT g % 500, g % 199 = 0 FROM generate_series(1, 1800) AS g;
ANALYZE pa;

CREATE PROCEDURE assert_same(label text, query_text text)
LANGUAGE plpgsql AS $$
DECLARE
    differences bigint;
BEGIN
    PERFORM set_config('bloompg.enable', 'on', true);
    EXECUTE 'CREATE TEMP TABLE bloompg_on AS ' || query_text;
    PERFORM set_config('bloompg.enable', 'off', true);
    EXECUTE 'CREATE TEMP TABLE bloompg_off AS ' || query_text;
    EXECUTE
        'SELECT count(*) FROM (' ||
        '(SELECT * FROM bloompg_on EXCEPT ALL SELECT * FROM bloompg_off) ' ||
        'UNION ALL ' ||
        '(SELECT * FROM bloompg_off EXCEPT ALL SELECT * FROM bloompg_on)' ||
        ') AS delta'
    INTO differences;
    IF differences <> 0 THEN
        RAISE EXCEPTION '% differs by % rows', label, differences;
    END IF;
    DROP TABLE bloompg_on;
    DROP TABLE bloompg_off;
END
$$;

CREATE PROCEDURE assert_plan_contains(label text, query_text text, needle text)
LANGUAGE plpgsql AS $$
DECLARE
    plan_line text;
    found boolean := false;
BEGIN
    PERFORM set_config('bloompg.enable', 'on', true);
    FOR plan_line IN EXECUTE
        'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || query_text
    LOOP
        found := found OR position(needle IN plan_line) > 0;
    END LOOP;
    IF NOT found THEN
        RAISE EXCEPTION '% did not contain plan node %', label, needle;
    END IF;
END
$$;

CREATE PROCEDURE assert_parallel_rescan_plan(label text, query_text text)
LANGUAGE plpgsql AS $$
DECLARE
    plan_json jsonb;
BEGIN
    PERFORM set_config('bloompg.enable', 'on', true);
    EXECUTE
        'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' ||
        query_text
    INTO plan_json;
    IF NOT jsonb_path_exists(plan_json, (
        '$.** ? (@."Node Type" == "Nested Loop")."Plans"[*] ? ' ||
        '(@."Parent Relationship" == "Inner" && ' ||
        '@."Node Type" == "Gather" && @."Actual Loops" > 1 && ' ||
        '@."Workers Launched" > 0)')::jsonpath) THEN
        RAISE EXCEPTION '% did not rescan a worker-backed inner Gather', label;
    END IF;
    IF NOT jsonb_path_exists(plan_json, (
        '$.** ? (@."Custom Plan Provider" == "BloomPG Memory" && ' ||
        '@."Snapshot Reuse" == "parallel DSM (replicated)" && ' ||
        '@."Actual Loops" > 1)')::jsonpath) THEN
        RAISE EXCEPTION '% did not rescan a replicated materialization', label;
    END IF;
END
$$;

CREATE PROCEDURE assert_parameter_same(label text, bound_value integer)
LANGUAGE plpgsql AS $$
DECLARE
    bloom_count bigint;
    native_count bigint;
BEGIN
    PERFORM set_config('bloompg.enable', 'on', true);
    EXECUTE
        'SELECT count(*) FROM a JOIN b ON a.k = b.k ' ||
        'WHERE a.keep AND a.k < $1'
    INTO bloom_count USING bound_value;
    PERFORM set_config('bloompg.enable', 'off', true);
    EXECUTE
        'SELECT count(*) FROM a JOIN b ON a.k = b.k ' ||
        'WHERE a.keep AND a.k < $1'
    INTO native_count USING bound_value;
    IF bloom_count IS DISTINCT FROM native_count THEN
        RAISE EXCEPTION '% differs: bloom %, native %',
            label, bloom_count, native_count;
    END IF;
END
$$;

CREATE PROCEDURE assert_scroll_cursor()
LANGUAGE plpgsql AS $$
DECLARE
    cursor_handle refcursor;
    row_value record;
    bloom_first jsonb;
    bloom_last jsonb;
    bloom_backward jsonb;
    native_first jsonb;
    native_last jsonb;
    native_backward jsonb;
    query_text text :=
        'SELECT a.k AS ak, a.k2 AS ak2, b.k2 AS bk2 ' ||
        'FROM a JOIN b ON a.k = b.k WHERE a.keep ' ||
        'ORDER BY 1 NULLS LAST, 2, 3';
BEGIN
    PERFORM set_config('bloompg.enable', 'on', true);
    cursor_handle := 'bloom_scroll';
    OPEN cursor_handle SCROLL FOR EXECUTE query_text;
    FETCH FIRST FROM cursor_handle INTO row_value;
    bloom_first := to_jsonb(row_value);
    FETCH LAST FROM cursor_handle INTO row_value;
    bloom_last := to_jsonb(row_value);
    MOVE BACKWARD 7 FROM cursor_handle;
    FETCH NEXT FROM cursor_handle INTO row_value;
    bloom_backward := to_jsonb(row_value);
    CLOSE cursor_handle;

    PERFORM set_config('bloompg.enable', 'off', true);
    cursor_handle := 'native_scroll';
    OPEN cursor_handle SCROLL FOR EXECUTE query_text;
    FETCH FIRST FROM cursor_handle INTO row_value;
    native_first := to_jsonb(row_value);
    FETCH LAST FROM cursor_handle INTO row_value;
    native_last := to_jsonb(row_value);
    MOVE BACKWARD 7 FROM cursor_handle;
    FETCH NEXT FROM cursor_handle INTO row_value;
    native_backward := to_jsonb(row_value);
    CLOSE cursor_handle;

    IF bloom_first IS DISTINCT FROM native_first OR
       bloom_last IS DISTINCT FROM native_last OR
       bloom_backward IS DISTINCT FROM native_backward THEN
        RAISE EXCEPTION 'scroll cursor results differ';
    END IF;
END
$$;

CREATE PROCEDURE run_differential_matrix()
LANGUAGE plpgsql AS $$
DECLARE
    i integer;
    query_text text;
BEGIN
    FOR i IN 0..59 LOOP
        CASE i % 5
        WHEN 0 THEN
            query_text := format(
                'SELECT a.k2, count(*), sum(b.k2) ' ||
                'FROM a JOIN b ON a.k = b.k ' ||
                'WHERE a.k2 <= %s AND b.k2 >= %s ' ||
                'AND (a.keep OR a.k < %s) GROUP BY a.k2',
                2 + i % 15, i % 8, 10 + i % 90);
        WHEN 1 THEN
            query_text := format(
                'SELECT a.k, count(*) FROM a JOIN b ' ||
                'ON a.k = b.k AND a.k2 = b.k2 ' ||
                'WHERE a.keep AND a.k < %s GROUP BY a.k',
                30 + (i * 17) % 470);
        WHEN 2 THEN
            query_text := format(
                'SELECT count(*), sum(a.k2) FROM a ' ||
                'JOIN b ON a.k = b.k JOIN c ON b.k = c.k ' ||
                'WHERE a.k2 < %s AND b.k2 >= %s AND c.flag',
                1 + i % 17, i % 12);
        WHEN 3 THEN
            query_text := format(
                'SELECT a.k2, count(b.k) FROM a LEFT JOIN b ON a.k = b.k ' ||
                'WHERE a.keep AND a.k2 <= %s GROUP BY a.k2',
                i % 17);
        ELSE
            query_text := format(
                'SELECT a.k, a.k2 FROM a WHERE a.keep AND a.k < %s ' ||
                'AND %s (SELECT 1 FROM b WHERE b.k = a.k AND b.k2 = a.k2)',
                20 + (i * 29) % 480,
                CASE WHEN i % 2 = 0 THEN 'EXISTS' ELSE 'NOT EXISTS' END);
        END CASE;
        CALL assert_same(format('matrix_%s', i), query_text);
    END LOOP;
END
$$;

SET client_min_messages = warning;

CALL assert_same('inner',
    'SELECT a.k, a.k2, b.payload FROM a JOIN b ON a.k = b.k WHERE a.keep');
CALL assert_same('composite',
    'SELECT a.k, a.k2, count(*) FROM a JOIN b ON a.k = b.k AND a.k2 = b.k2 WHERE a.keep GROUP BY a.k, a.k2');
CALL assert_same('chain',
    'SELECT count(*), sum(a.k2) FROM a JOIN b ON a.k = b.k JOIN c ON b.k = c.k WHERE a.keep AND c.flag');
CALL assert_same('text_and_date',
    'SELECT a.t, count(*) FROM a JOIN b ON a.t COLLATE "C" = b.t COLLATE "C" AND a.d = b.d WHERE a.keep GROUP BY a.t');
CALL assert_same('left_outer',
    'SELECT a.k AS ak, b.k AS bk, b.k2 FROM a LEFT JOIN b ON a.k = b.k WHERE a.keep');
CALL assert_same('full_outer',
    'SELECT coalesce(a.k, b.k::int) AS k, count(*) FROM a FULL JOIN b ON a.k = b.k WHERE a.keep IS TRUE OR b.k < 8 GROUP BY 1');
CALL assert_same('semi',
    'SELECT a.k, a.k2 FROM a WHERE a.keep AND EXISTS (SELECT 1 FROM b WHERE b.k = a.k AND b.k2 = a.k2)');
CALL assert_same('anti',
    'SELECT a.k, a.k2 FROM a WHERE a.keep AND NOT EXISTS (SELECT 1 FROM b WHERE b.k = a.k AND b.k2 = a.k2)');
CALL assert_same('self_join',
    'SELECT x.k, count(*) FROM a x JOIN a y ON x.k = y.k WHERE x.keep AND y.k2 < 4 GROUP BY x.k');
CALL assert_same('materialized_cte',
    'WITH x AS MATERIALIZED (SELECT k, k2, payload FROM b WHERE k < 300) SELECT count(*), sum(length(x.payload)) FROM a JOIN x ON a.k = x.k WHERE a.keep');
CALL assert_same('cte_aliases',
    'WITH x AS MATERIALIZED (SELECT k, k2 FROM b WHERE k < 350) SELECT count(*) FROM a JOIN x x1 ON a.k = x1.k JOIN x x2 ON a.k = x2.k WHERE a.keep AND x1.k2 = x2.k2');
CALL assert_same('subquery_distinct',
    'SELECT count(*) FROM a JOIN (SELECT DISTINCT k FROM b WHERE k < 400) s ON a.k = s.k WHERE a.keep');
CALL assert_same('subquery_group',
    'SELECT sum(s.n) FROM a JOIN (SELECT k, count(*) n FROM b GROUP BY k) s ON a.k = s.k WHERE a.keep');
CALL assert_same('subquery_limit',
    'SELECT count(*) FROM a JOIN (SELECT k, k2 FROM b ORDER BY k, k2 LIMIT 300 OFFSET 10) s ON a.k = s.k WHERE a.keep');
CALL assert_same('partition',
    'SELECT pa.k, count(*) FROM pa JOIN b ON pa.k = b.k WHERE pa.keep GROUP BY pa.k');
CREATE VIEW a_view AS
SELECT k, k2, keep, payload FROM a;
CREATE MATERIALIZED VIEW b_mat AS
SELECT k, k2, payload FROM b WHERE k < 500;
ANALYZE b_mat;
CALL assert_same('view',
    'SELECT count(*) FROM a_view v JOIN b ON v.k = b.k WHERE v.keep');
CALL assert_plan_contains('view_plan',
    'SELECT count(*) FROM a_view v JOIN b ON v.k = b.k WHERE v.keep',
    'Custom Scan (BloomPG');
CALL assert_same('materialized_view',
    'SELECT count(*) FROM a JOIN b_mat m ON a.k = m.k WHERE a.keep');
CALL assert_plan_contains('materialized_view_plan',
    'SELECT count(*) FROM a JOIN b_mat m ON a.k = m.k WHERE a.keep',
    'Custom Scan (BloomPG');
CALL assert_same('set_operation',
    'SELECT k FROM a WHERE keep UNION ALL SELECT k::int FROM b WHERE k < 5');
CALL assert_parameter_same('parameter_10', 10);
CALL assert_parameter_same('parameter_100', 100);
CALL assert_parameter_same('parameter_1000', 1000);
CALL assert_scroll_cursor();

SET enable_hashjoin = off;
SET enable_mergejoin = off;
CALL assert_same('nested_loop_execution',
    'SELECT a.k, a.k2, b.payload FROM a JOIN b ON a.k = b.k WHERE a.keep');
CALL assert_plan_contains('nested_loop_plan',
    'SELECT count(*) FROM a JOIN b ON a.k = b.k WHERE a.keep',
    'Nested Loop');
RESET enable_hashjoin;
RESET enable_mergejoin;

SET enable_hashjoin = off;
SET enable_nestloop = off;
CALL assert_same('merge_join_execution',
    'SELECT a.k, a.k2, b.payload FROM a JOIN b ON a.k = b.k WHERE a.keep');
CALL assert_plan_contains('merge_join_plan',
    'SELECT count(*) FROM a JOIN b ON a.k = b.k WHERE a.keep',
    'Merge Join');
RESET enable_hashjoin;
RESET enable_nestloop;
CALL run_differential_matrix();

SET bloompg.sample_mode = 'prepared';
CALL assert_same('prepared_sample',
    'SELECT count(*) FROM a JOIN b ON a.k = b.k WHERE a.keep');
CALL assert_same('prepared_subplan_qual',
    'SELECT count(*) FROM a JOIN b ON a.k = b.k WHERE a.keep AND b.k NOT IN (SELECT c.k FROM c WHERE c.flag)');
SET bloompg.enable = on;
SELECT count(*) > 0 AS prepared_sample_query_ok
FROM a JOIN b ON a.k = b.k
WHERE a.keep;
SELECT bloompg_last_trace() LIKE '%(cached)%' AS prepared_sample_cache_hit;
SELECT bloompg_last_trace() LIKE '%samplemem=%cache=hit%'
    AS prepared_tuple_cache_hit;
INSERT INTO a VALUES
    (777, 1, 'cached-dml', date '2024-01-01', true, repeat('x', 5000));
INSERT INTO b VALUES
    (777, 1, 'cached-dml', date '2024-01-01', repeat('y', 5000));
CALL assert_same('prepared_sample_after_dml',
    'SELECT count(*), max(length(a.payload)) FROM a JOIN b ON a.k = b.k WHERE a.keep');
SET bloompg.sample_mode = 'instant';

SET bloompg.enable = on;
SELECT count(*) > 0 AS cache_query_ok
FROM a
JOIN c AS c1 ON a.k = c1.k
JOIN c AS c2 ON a.k = c2.k
WHERE a.keep;
SELECT bloompg_last_trace() LIKE '%cache=hit%' AS filter_cache_reuse_ok;

-- Exact equality domains retain only NDV and a version.  Once a relation has
-- consumed the current version, an equal outgoing NDV proves that its key set
-- is unchanged and redundant transfers around this triangle can be skipped.
CREATE TABLE domain_seed(k integer, keep boolean);
CREATE TABLE domain_left(k integer, payload integer);
CREATE TABLE domain_right(k integer, payload integer);
INSERT INTO domain_seed
SELECT g, g <= 5 FROM generate_series(1, 1000) AS g;
INSERT INTO domain_left
SELECT 1 + (g % 200), g FROM generate_series(1, 2000) AS g;
INSERT INTO domain_right
SELECT 1 + (g % 200), g FROM generate_series(1, 2000) AS g;
ANALYZE domain_seed, domain_left, domain_right;
SET bloompg.excitation_threshold = 1.0;
SET bloompg.profile = on;
SET bloompg.profile_log = off;
CALL assert_same('equality_domain_cycle',
    'SELECT count(*) FROM domain_seed s JOIN domain_left l ON s.k = l.k JOIN domain_right r ON s.k = r.k AND l.k = r.k WHERE s.keep');
SET bloompg.enable = on;
SELECT count(*) = 500 AS equality_domain_query_ok
FROM domain_seed s
JOIN domain_left l ON s.k = l.k
JOIN domain_right r ON s.k = r.k AND l.k = r.k
WHERE s.keep;
SELECT COALESCE(
    bloompg_last_profile()->'transfer'->'summary'->>'progress_metric' = 'ndv'
    AND (bloompg_last_profile()->'transfer'->'summary'
        ->>'equality_domain_suppressed')::integer > 0
    AND (bloompg_last_profile()->'transfer'->'summary'
        ->>'equality_domain_observations')::integer > 0
    AND (bloompg_last_profile()->'transfer'->'summary'
        ->>'equality_domain_uncontained')::integer = 0,
    false) AS equality_domain_size_only_ok;

-- Size containment is ambiguous if one relation contributes two attributes
-- to the same equality class, so that shape must retain normal propagation.
CREATE TABLE domain_multi(k1 integer, k2 integer, keep boolean);
INSERT INTO domain_multi
SELECT g, g, g <= 5 FROM generate_series(1, 1000) AS g;
ANALYZE domain_multi;
CALL assert_same('equality_domain_multi_column',
    'SELECT count(*) FROM domain_multi m JOIN domain_left l ON m.k1 = l.k JOIN domain_right r ON m.k2 = r.k AND l.k = r.k WHERE m.keep');
SET bloompg.enable = on;
SELECT count(*) = 500 AS equality_domain_multi_column_query_ok
FROM domain_multi m
JOIN domain_left l ON m.k1 = l.k
JOIN domain_right r ON m.k2 = r.k AND l.k = r.k
WHERE m.keep;
SELECT COALESCE(
    (bloompg_last_profile()->'transfer'->'summary'
        ->>'equality_domain_classes')::integer = 0
    AND (bloompg_last_profile()->'transfer'->'summary'
        ->>'equality_domain_suppressed')::integer = 0,
    false) AS equality_domain_multi_column_fallback_ok;
SET bloompg.profile = off;
RESET bloompg.profile_log;

CREATE TABLE filter_src(k integer, keep boolean);
CREATE TABLE filter_dst(k integer, payload text);
INSERT INTO filter_src
SELECT g, g <= 100 FROM generate_series(1, 1000) AS g;
INSERT INTO filter_dst
SELECT 1 + (g % 500), repeat('f', 96) FROM generate_series(1, 2000) AS g;
ANALYZE filter_src, filter_dst;
SET bloompg.excitation_threshold = 0.15;
CALL assert_same('direct_filter',
    'SELECT count(*), sum(length(d.payload)) FROM filter_src s JOIN filter_dst d ON s.k = d.k WHERE s.keep');
CALL assert_plan_contains('direct_filter_plan',
    'SELECT count(*) FROM filter_src s JOIN filter_dst d ON s.k = d.k WHERE s.keep',
    'Custom Scan (BloomPG Filter)');
SELECT bloompg_last_trace() LIKE '%filtered=%'
   AND bloompg_last_trace() LIKE '%kind=bitmap%' AS direct_filter_trace_ok;

CREATE INDEX filter_dst_k_idx ON filter_dst(k);
ANALYZE filter_dst;

-- FILTER is a PathSet transform.  These data shapes let the unmodified core
-- cost model naturally select each access-path family; no enable_* switch is
-- used to manufacture the plans.
CREATE TABLE p1_outer_index(k integer, keep boolean);
CREATE TABLE p1_outer_param(k integer, keep boolean);
CREATE TABLE p1_probe(k integer, payload integer, filler text);
CREATE TABLE p1_param_probe(k integer, payload integer, filler text);
INSERT INTO p1_outer_index
SELECT CASE WHEN g % 10 = 0 THEN g / 10 ELSE 999999 END, g % 10 = 0
FROM generate_series(1, 50000) AS g;
INSERT INTO p1_outer_param
SELECT g, g = 1 FROM generate_series(1, 1000) AS g;
INSERT INTO p1_probe
SELECT 1 + (g % 25000), 1 + (g % 25000), repeat('i', 128)
FROM generate_series(1, 50000) AS g;
INSERT INTO p1_param_probe
SELECT 1 + (g % 200), 1 + (g % 200), repeat('p', 96)
FROM generate_series(1, 200000) AS g;
CREATE INDEX p1_probe_k_idx ON p1_probe(k);
CREATE INDEX p1_param_probe_k_idx ON p1_param_probe(k);
CLUSTER p1_probe USING p1_probe_k_idx;
VACUUM (ANALYZE) p1_probe;
ANALYZE p1_outer_index, p1_outer_param, p1_param_probe;

SET bloompg.index_transfer = off;
SET bloompg.profile = on;
SET bloompg.profile_log = off;
CALL assert_same('p1_plain_index',
    'SELECT sum(i.payload) FROM p1_outer_index o JOIN p1_probe i ON o.k = i.k WHERE o.keep AND i.k <= 6500');
CALL assert_plan_contains('p1_plain_index_plan',
    'SELECT sum(i.payload) FROM p1_outer_index o JOIN p1_probe i ON o.k = i.k WHERE o.keep AND i.k <= 6500',
    'Wrapped Access Path: Index Scan');
CALL assert_plan_contains('p1_plain_index_name',
    'SELECT sum(i.payload) FROM p1_outer_index o JOIN p1_probe i ON o.k = i.k WHERE o.keep AND i.k <= 6500',
    'p1_probe_k_idx');
CALL assert_same('p1_index_only',
    'SELECT count(*) FROM p1_outer_index o JOIN p1_probe i ON o.k = i.k WHERE o.keep AND i.k <= 6500');
CALL assert_plan_contains('p1_index_only_plan',
    'SELECT count(*) FROM p1_outer_index o JOIN p1_probe i ON o.k = i.k WHERE o.keep AND i.k <= 6500',
    'Wrapped Access Path: Index Only Scan');

SET bloompg.excitation_threshold = 0.002;
CALL assert_same('p1_parameterized_index_rescan',
    'SELECT o.k, sum(i.payload) FROM p1_outer_param o JOIN p1_param_probe i ON o.k = i.k WHERE o.keep GROUP BY o.k');
CALL assert_plan_contains('p1_parameterized_index_filter',
    'SELECT sum(i.payload) FROM p1_outer_param o JOIN p1_param_probe i ON o.k = i.k WHERE o.keep',
    'Custom Scan (BloomPG Filter)');
CALL assert_plan_contains('p1_parameterized_index_path',
    'SELECT sum(i.payload) FROM p1_outer_param o JOIN p1_param_probe i ON o.k = i.k WHERE o.keep',
    'p1_param_probe_k_idx');
SELECT COALESCE(
    (bloompg_last_profile()->'planning'->'path_transform'->>'parameterized')::integer > 0
    AND (bloompg_last_profile()->'planning'->'path_transform'->>'input_paths')::integer =
        (bloompg_last_profile()->'planning'->'path_transform'->>'wrapped_paths')::integer,
    false) AS parameterized_pathset_preserved_ok;
SET bloompg.excitation_threshold = 0.15;

-- A local-qual-only Memory replacement remains semantically equivalent to
-- its native parameterized paths.  P1 must be allowed to choose keyed lookup
-- when the outer side is tiny instead of rescanning the column store.
CREATE TABLE p1_memory_outer(k integer);
CREATE TABLE p1_memory_probe(k integer, keep boolean, payload integer);
INSERT INTO p1_memory_outer SELECT g FROM generate_series(1, 10) AS g;
INSERT INTO p1_memory_probe
SELECT g, g <= 20000, g FROM generate_series(1, 200000) AS g;
CREATE UNIQUE INDEX p1_memory_probe_k_idx ON p1_memory_probe(k);
ANALYZE p1_memory_outer, p1_memory_probe;
CALL assert_same('p1_memory_parameterized_index',
    'SELECT sum(p.payload) FROM p1_memory_outer o JOIN p1_memory_probe p ON o.k = p.k WHERE p.keep');
CALL assert_plan_contains('p1_memory_parameterized_index_plan',
    'SELECT sum(p.payload) FROM p1_memory_outer o JOIN p1_memory_probe p ON o.k = p.k WHERE p.keep',
    'p1_memory_probe_k_idx');
SELECT COALESCE(
    (bloompg_last_profile()->'planning'->'path_transform'
        ->>'replacement_parameterized_retained')::integer > 0
    AND EXISTS (
        SELECT 1
        FROM jsonb_array_elements(
            bloompg_last_profile()->'transfer'->'relations') AS relation
        WHERE relation->>'alias' = 'p'
          AND relation->>'replacement' = 'memory'),
    false) AS p1_memory_parameterized_path_preserved_ok;

CREATE TABLE p1_scan_source(k integer, keep boolean);
CREATE TABLE p1_scan_probe(k integer, flag integer, payload text);
INSERT INTO p1_scan_source
SELECT CASE WHEN g % 100 = 0 THEN g / 100 ELSE 999999 END, g % 100 = 0
FROM generate_series(1, 100000) AS g;
INSERT INTO p1_scan_probe
SELECT 1 + (g % 2500), (g * 37) % 10000, repeat('b', 128)
FROM generate_series(1, 50000) AS g;
CREATE INDEX p1_scan_probe_flag_idx ON p1_scan_probe(flag);
ANALYZE p1_scan_source, p1_scan_probe;
SET bloompg.excitation_threshold = 0.02;
CALL assert_same('p1_seq_filter',
    'SELECT count(*) FROM p1_scan_source s JOIN p1_scan_probe p ON s.k = p.k WHERE s.keep');
CALL assert_plan_contains('p1_seq_filter_plan',
    'SELECT count(*) FROM p1_scan_source s JOIN p1_scan_probe p ON s.k = p.k WHERE s.keep',
    'Wrapped Access Path: Seq Scan');
CALL assert_same('p1_bitmap_filter',
    'SELECT sum(length(p.payload)) FROM p1_scan_source s JOIN p1_scan_probe p ON s.k = p.k WHERE s.keep AND p.flag < 1000');
CALL assert_plan_contains('p1_bitmap_filter_plan',
    'SELECT sum(length(p.payload)) FROM p1_scan_source s JOIN p1_scan_probe p ON s.k = p.k WHERE s.keep AND p.flag < 1000',
    'Wrapped Access Path: Bitmap Heap Scan');
SET bloompg.profile = off;
RESET bloompg.profile_log;
RESET bloompg.index_transfer;

CREATE INDEX filter_src_k_idx ON filter_src(k);
ANALYZE filter_src;
SET bloompg.excitation_threshold = 1.0;
SET bloompg.index_transfer = on;
SET bloompg.index_transfer_max_fraction = 1.0;
SET bloompg.index_guard_min_rows = 0;
SET bloompg.transfer_progress_metric = 'rows';
SET bloompg.profile = on;
SET bloompg.profile_log = off;
SELECT count(*) = 400 AS index_transfer_query_ok
FROM filter_src s JOIN filter_dst d ON s.k = d.k
WHERE s.keep;
SELECT COALESCE((
    SELECT (
        (relation->'materialization'->'index_probe'->>'scans')::integer > 0
        AND (relation->'materialization'->'index_probe'->>'batches')::integer > 0
        AND (relation->'materialization'->'index_probe'->>'keys')::integer = 100
    ) OR (
        (relation->'materialization'->'access_plan'->>'exact_keys')::integer = 100
        AND (
            (relation->'materialization'->'access_plan'->>'index_scans')::integer > 0
            OR (relation->'materialization'->'access_plan'->>'index_only_scans')::integer > 0
            OR (relation->'materialization'->'access_plan'->>'bitmap_heap_scans')::integer > 0
        )
    )
    FROM jsonb_array_elements(
        bloompg_last_profile()->'transfer'->'relations') AS relation
    WHERE relation->>'alias' = 'd'
), false) AS index_transfer_profile_ok;
SELECT EXISTS (
        SELECT 1
        FROM jsonb_array_elements(
            bloompg_last_profile()->'transfer'->'steps') AS step
        WHERE step->>'source' = 's'
          AND (step->>'selection_guard_fallback')::boolean
    ) AND EXISTS (
        SELECT 1
        FROM jsonb_array_elements(
            bloompg_last_profile()->'transfer'->'steps') AS step
        WHERE step->>'source' = 'd'
          AND (step->>'selection_index_ready')::boolean
    ) AND bloompg_last_profile()->'transfer'->'summary'
        ->>'progress_metric' = 'rows' AS index_wait_guard_profile_ok;

-- Index readiness controls the physical access path, not fixed-point
-- participation.  If every source is waiting, the smallest guarded relation
-- must be materialized even when its current filter targets an unindexed
-- column, so it can publish the later indexed-key domain.
CREATE TABLE guard_seed(k integer, keep boolean);
CREATE TABLE guard_middle(seed_key integer, indexed_key integer);
CREATE TABLE guard_tail(indexed_key integer);
INSERT INTO guard_seed
SELECT g, g = 1 FROM generate_series(1, 100) AS g;
INSERT INTO guard_middle
SELECT g % 10, g FROM generate_series(1, 1000) AS g;
INSERT INTO guard_tail SELECT g FROM generate_series(1, 1000) AS g;
CREATE INDEX guard_middle_indexed_key_idx
    ON guard_middle(indexed_key);
ANALYZE guard_seed, guard_middle, guard_tail;
SET bloompg.index_transfer_max_keys = 50;
CALL assert_same('index_guard_wait_break',
    'SELECT count(*) FROM guard_seed s JOIN guard_middle m ON s.k = m.seed_key JOIN guard_tail t ON m.indexed_key = t.indexed_key WHERE s.keep');
SET bloompg.enable = on;
SELECT count(*) = 100 AS index_guard_wait_break_query_ok
FROM guard_seed s
JOIN guard_middle m ON s.k = m.seed_key
JOIN guard_tail t ON m.indexed_key = t.indexed_key
WHERE s.keep;
SELECT COALESCE(
    NOT (bloompg_last_profile()->'transfer'->'summary'
        ->>'native_fallback')::boolean
    AND (bloompg_last_profile()->'transfer'->'summary'
        ->>'applied')::boolean
    AND EXISTS (
        SELECT 1
        FROM jsonb_array_elements(
            bloompg_last_profile()->'transfer'->'relations') AS relation
        WHERE relation->>'alias' = 'm'
          AND relation->>'replacement' = 'memory'
          AND (relation->'materialization'->'selection'
              ->>'fallback')::boolean
          AND (relation->'materialization'->>'exact_scans')::integer > 0),
    false) AS index_guard_wait_break_profile_ok;

-- A native parameterized path must not truncate propagation in a wider graph.
-- When the exact access planner rejects the key population, the scheduler
-- falls back to one physical scan and continues the equality-domain transfer.
CREATE TABLE protected_dimension(id integer, keep boolean);
CREATE TABLE protected_source(id integer, dimension_id integer);
CREATE TABLE guarded_dimension(id integer, keep boolean);
CREATE TABLE protected_guarded_fact(protected_id integer, guarded_id integer);
INSERT INTO protected_dimension
SELECT g, g < 9 FROM generate_series(0, 9) AS g;
INSERT INTO protected_source
SELECT g, g % 10 FROM generate_series(1, 10000) AS g;
INSERT INTO guarded_dimension
SELECT g, g < 2 FROM generate_series(0, 999) AS g;
INSERT INTO protected_guarded_fact
SELECT 1 + (g % 10000), g % 1000 FROM generate_series(1, 100000) AS g;
CREATE UNIQUE INDEX protected_source_id_idx ON protected_source(id);
CREATE INDEX protected_source_dimension_idx
    ON protected_source(dimension_id);
CREATE INDEX protected_guarded_fact_protected_idx
    ON protected_guarded_fact(protected_id);
CREATE INDEX protected_guarded_fact_guarded_idx
    ON protected_guarded_fact(guarded_id);
ANALYZE protected_dimension, protected_source, guarded_dimension,
    protected_guarded_fact;
SET bloompg.index_transfer_max_fraction = 0.15;
SET bloompg.index_transfer_max_keys = 50;
SET bloompg.index_transfer_planner_max_keys = 1;
CALL assert_same('index_guard_complete_transfer',
    'SELECT count(*) FROM protected_dimension d JOIN protected_source p ON d.id = p.dimension_id JOIN protected_guarded_fact f ON p.id = f.protected_id JOIN guarded_dimension g ON g.id = f.guarded_id WHERE d.keep AND g.keep');
SET bloompg.enable = on;
SELECT count(*) = 200 AS index_guard_complete_transfer_query_ok
FROM protected_dimension d
JOIN protected_source p ON d.id = p.dimension_id
JOIN protected_guarded_fact f ON p.id = f.protected_id
JOIN guarded_dimension g ON g.id = f.guarded_id
WHERE d.keep AND g.keep;
SELECT COALESCE(
    NOT (bloompg_last_profile()->'transfer'->'summary'
        ->>'native_fallback')::boolean
    AND (bloompg_last_profile()->'transfer'->'summary'
        ->>'applied')::boolean
    AND EXISTS (
        SELECT 1
        FROM jsonb_array_elements(
            bloompg_last_profile()->'transfer'->'relations') AS relation
        WHERE relation->>'alias' = 'f'
          AND relation->>'replacement' = 'memory'
          AND (relation->'materialization'->>'exact_scans')::integer > 0),
    false) AS index_guard_complete_transfer_profile_ok;
SET bloompg.profile = off;
RESET bloompg.profile_log;
RESET bloompg.transfer_progress_metric;
RESET bloompg.index_guard_min_rows;
RESET bloompg.index_transfer_max_fraction;
RESET bloompg.index_transfer_max_keys;
RESET bloompg.index_transfer_planner_max_keys;
RESET bloompg.index_transfer;

SET bloompg.excitation_threshold = 0.0;
SET bloompg.profile = on;
SET bloompg.profile_log = off;
SELECT count(*) > 0 AS native_fallback_query_ok
FROM a JOIN b ON a.k = b.k
WHERE a.keep;
SELECT (bloompg_last_profile()->'transfer'->'summary'->>'applied')::boolean = false
   AND (bloompg_last_profile()->'planning'->>'replanned')::boolean
   AND (bloompg_last_profile()->'planning'->>'native_preview_ms')::numeric >= 0
   AS native_fallback_replanned_ok;
SET bloompg.profile = off;
RESET bloompg.profile_log;
SET bloompg.excitation_threshold = 1.0;

CREATE TABLE exact_empty_src(k integer, keep boolean);
CREATE TABLE exact_empty_dst(k integer, payload text);
INSERT INTO exact_empty_src
SELECT g, g <= 10 FROM generate_series(1, 1000) AS g;
INSERT INTO exact_empty_dst
SELECT 10000 + g, repeat('e', 96) FROM generate_series(1, 2000) AS g;
ANALYZE exact_empty_src, exact_empty_dst;
CALL assert_same('exact_empty',
    'SELECT count(*) FROM exact_empty_src s JOIN exact_empty_dst d ON s.k = d.k WHERE s.keep');
CALL assert_plan_contains('exact_empty_plan',
    'SELECT count(*) FROM exact_empty_src s JOIN exact_empty_dst d ON s.k = d.k WHERE s.keep',
    'Custom Scan (BloomPG Empty)');
SELECT bloompg_last_trace() LIKE '%empty=%' AS exact_empty_trace_ok;

PREPARE stale_empty AS
SELECT count(*) FROM exact_empty_src s
JOIN exact_empty_dst d ON s.k = d.k
WHERE s.keep;
EXECUTE stale_empty;
INSERT INTO exact_empty_dst VALUES (1, 'now visible');
EXECUTE stale_empty;
DEALLOCATE stale_empty;

CREATE TABLE empty_side(k integer);
CREATE TABLE error_side(k integer, divisor integer);
INSERT INTO error_side VALUES (1, 0), (2, 1);
ANALYZE empty_side, error_side;
CALL assert_same('optimizer_error_fallback',
    'SELECT count(*) FROM empty_side e JOIN error_side z ON e.k = z.k WHERE 10 / z.divisor > 0');

SET bloompg.replan_after_transfer = off;
CALL assert_same('direct_p0_execution',
    'SELECT a.k, a.k2, b.payload FROM a JOIN b ON a.k = b.k WHERE a.keep');
SET bloompg.replan_after_transfer = on;

SET bloompg.materialization_memory = '64kB';
SET bloompg.late_materialize = off;
CALL assert_same('memory_budget_fallback',
    'SELECT count(*), sum(length(a.payload)) FROM a JOIN b ON a.k = b.k WHERE a.k < 200');
SELECT bloompg_last_trace() LIKE
    '%fallback=optimizer_execution_error sqlstate=54000%'
    AS memory_budget_fallback_ok;
CALL assert_same('memory_budget_fallback_repeat',
    'SELECT count(*), sum(length(a.payload)) FROM a JOIN b ON a.k = b.k WHERE a.k < 200');
RESET bloompg.materialization_memory;
SET bloompg.late_materialize = on;

SET bloompg.enable = on;
SELECT count(*) > 0 AND sum(length(a.payload)) > 0 AS wide_result_ok
FROM a JOIN b ON a.k = b.k
WHERE a.keep;
SELECT bloompg_last_trace() LIKE '%transfers=%' AS transfer_trace_ok;

SET max_parallel_workers_per_gather = 4;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
CALL assert_same('parallel_wide',
    'SELECT a.k, a.k2, b.payload FROM a JOIN b ON a.k = b.k WHERE a.keep');
CALL assert_same('parallel_composite',
    'SELECT a.k, count(*) FROM a JOIN b ON a.k = b.k AND a.k2 = b.k2 WHERE a.keep GROUP BY a.k');
CALL assert_same('parallel_partition',
    'SELECT count(*) FROM pa JOIN b ON pa.k = b.k WHERE pa.keep');
CALL assert_plan_contains('parallel_plan',
    'SELECT count(*) FROM a JOIN b ON a.k = b.k WHERE a.keep',
    'Parallel Custom Scan (BloomPG');

-- A partial join can use a regular replacement as its replicated inner side,
-- while a higher nested loop repeatedly launches an inner Gather.  Both the
-- worker-local cursor and the Gather-wide shared cursor must restart at their
-- own lifecycle boundary.
CREATE TABLE parallel_rescan_gate(kind integer, keep boolean);
CREATE TABLE parallel_rescan_hub(k integer, kind integer, keep boolean);
CREATE TABLE parallel_rescan_fact(k integer, payload integer);
INSERT INTO parallel_rescan_gate
SELECT g, g = 1 FROM generate_series(1, 200) AS g;
INSERT INTO parallel_rescan_hub
SELECT g, 1 + (g % 100), g <= 200 FROM generate_series(1, 1000) AS g;
INSERT INTO parallel_rescan_fact
SELECT 1 + (g % 1000), g FROM generate_series(1, 5000) AS g;
ANALYZE parallel_rescan_gate, parallel_rescan_hub, parallel_rescan_fact;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
CALL assert_same('parallel_nested_loop_rescan',
    'SELECT g.kind, h.k, f.payload FROM parallel_rescan_gate g JOIN parallel_rescan_hub h ON g.kind = h.kind JOIN parallel_rescan_fact f ON h.k = f.k WHERE g.keep AND h.keep');
CALL assert_parallel_rescan_plan('parallel_nested_loop_rescan_plan',
    'SELECT g.kind, h.k, f.payload FROM parallel_rescan_gate g JOIN parallel_rescan_hub h ON g.kind = h.kind JOIN parallel_rescan_fact f ON h.k = f.k WHERE g.keep AND h.keep');
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

CREATE TABLE parallel_filter_src(k integer, keep boolean);
CREATE TABLE parallel_filter_dst(k integer, payload text);
INSERT INTO parallel_filter_src
SELECT g, g <= 5 FROM generate_series(1, 1000) AS g;
INSERT INTO parallel_filter_dst
SELECT 1 + (g % 200), repeat('p', 96) FROM generate_series(1, 20000) AS g;
ANALYZE parallel_filter_src, parallel_filter_dst;
SET bloompg.excitation_threshold = 0.01;
SET parallel_leader_participation = off;
CALL assert_same('parallel_direct_filter',
    'SELECT count(*) FROM parallel_filter_src s JOIN parallel_filter_dst d ON s.k = d.k WHERE s.keep');
CALL assert_plan_contains('parallel_direct_filter_plan',
    'SELECT count(*) FROM parallel_filter_src s JOIN parallel_filter_dst d ON s.k = d.k WHERE s.keep',
    'Parallel Filter DSM: true');
RESET parallel_leader_participation;
SET bloompg.excitation_threshold = 1.0;

CREATE TABLE parallel_wave_hub(k integer, keep boolean);
CREATE TABLE parallel_wave_left(k integer, payload integer);
CREATE TABLE parallel_wave_right(k integer, payload integer);
INSERT INTO parallel_wave_hub
SELECT g, g <= 5 FROM generate_series(1, 1000) AS g;
INSERT INTO parallel_wave_left
SELECT 1 + (g % 200), g FROM generate_series(1, 20000) AS g;
INSERT INTO parallel_wave_right
SELECT 1 + (g % 200), g FROM generate_series(1, 20000) AS g;
ANALYZE parallel_wave_hub, parallel_wave_left, parallel_wave_right;
SET bloompg.transfer_parallel_min_rows = 0;
SET bloompg.transfer_workers = 4;
SET bloompg.profile = on;
SET bloompg.profile_log = off;
CALL assert_same('parallel_transfer_wave',
    'SELECT count(*) FROM parallel_wave_hub h JOIN parallel_wave_left l USING (k) JOIN parallel_wave_right r USING (k) WHERE h.keep');
SET bloompg.enable = on;
SELECT count(*) = 50000 AS parallel_wave_query_ok
FROM parallel_wave_hub h
JOIN parallel_wave_left l USING (k)
JOIN parallel_wave_right r USING (k)
WHERE h.keep;
SELECT (bloompg_last_profile()->'transfer'->'summary'->>'parallel_append_waves')::integer >= 1
   AND (bloompg_last_profile()->'transfer'->'summary'->>'parallel_waves')::integer
       < (bloompg_last_profile()->'transfer'->'summary'->>'parallel_scans')::integer
   AND (bloompg_last_profile()->'transfer'->'summary'
       ->>'parallel_workers_launched')::integer > 0
   AND (
       SELECT count(*)
       FROM jsonb_array_elements(
           bloompg_last_profile()->'transfer'->'relations') AS relation
       WHERE relation->'materialization'->'wave'->>'kind' = 'parallel-append'
         AND relation->'materialization'->'wave'->>'layout' = 'compact'
   ) = 2 AS parallel_wave_profile_ok;

-- A SubLink can restrict the whole query without making unrelated detached
-- relation scans unsafe.  Those local scans must retain parallel transfer.
CALL assert_same('parallel_transfer_restricted_query',
    'SELECT count(*) FROM parallel_wave_hub h JOIN parallel_wave_left l USING (k) JOIN parallel_wave_right r USING (k) WHERE h.keep AND h.k <= (SELECT max(h2.k) FROM parallel_wave_hub h2 WHERE h2.k = h.k)');
SET bloompg.enable = on;
SELECT count(*) = 50000 AS parallel_restricted_query_ok
FROM parallel_wave_hub h
JOIN parallel_wave_left l USING (k)
JOIN parallel_wave_right r USING (k)
WHERE h.keep
  AND h.k <= (SELECT max(h2.k) FROM parallel_wave_hub h2
              WHERE h2.k = h.k);
SELECT (bloompg_last_profile()->'transfer'->'summary'
       ->>'parallel_workers_launched')::integer > 0
    AS parallel_restricted_source_scan_ok;

-- Exercise the worker packet path itself, including NULL and toasted varlena
-- values.  The leader should import vectors without reconstructing slots.
CREATE TABLE parallel_pack_hub(k integer, keep boolean);
CREATE TABLE parallel_pack_source(k integer, payload text);
INSERT INTO parallel_pack_hub
SELECT g, g <= 5 FROM generate_series(1, 1000) AS g;
INSERT INTO parallel_pack_source
SELECT 1 + (g % 200),
       CASE WHEN g % 113 = 0 THEN NULL
            WHEN g % 1000 = 0 THEN repeat('x', 5000)
            ELSE repeat('x', 1 + (g % 7)) END
FROM generate_series(1, 320000) AS g;
ANALYZE parallel_pack_hub, parallel_pack_source;
CALL assert_same('parallel_transfer_vector_packet',
    'SELECT sum(length(s.payload)) > 0 FROM parallel_pack_hub h JOIN parallel_pack_source s USING (k) WHERE h.keep');
SET bloompg.enable = on;
SELECT sum(length(s.payload)) > 0 AS parallel_packet_query_ok
FROM parallel_pack_hub h
JOIN parallel_pack_source s USING (k)
WHERE h.keep;
SELECT COALESCE((
    SELECT relation->'materialization'->>'access_path' = 'parallel-p0'
       AND (relation->'materialization'->'parallel'
            ->>'workers_launched')::integer > 0
       AND (relation->'materialization'->'store'
            ->>'varlen_values')::integer > 0
       AND (relation->'materialization'->'store'
            ->>'slot_reconstructions')::integer = 0
    FROM jsonb_array_elements(
        bloompg_last_profile()->'transfer'->'relations') AS relation
    WHERE relation->>'alias' = 's'
), false) AS parallel_packet_profile_ok;

CREATE TABLE heterogeneous_wave_hub(k integer, keep boolean);
CREATE TABLE heterogeneous_wave_left(k integer, payload text);
CREATE TABLE heterogeneous_wave_right(k integer, payload bigint);
INSERT INTO heterogeneous_wave_hub
SELECT g, g <= 5 FROM generate_series(1, 1000) AS g;
INSERT INTO heterogeneous_wave_left
SELECT 1 + (g % 200),
       CASE WHEN g % 113 = 0 THEN NULL
            WHEN g % 10 = 0 THEN repeat('x', 5000)
            ELSE repeat('x', 1 + (g % 7)) END
FROM generate_series(1, 20000) AS g;
INSERT INTO heterogeneous_wave_right
SELECT 1 + (g % 200), g FROM generate_series(1, 20000) AS g;
ANALYZE heterogeneous_wave_hub, heterogeneous_wave_left,
    heterogeneous_wave_right;
CALL assert_same('heterogeneous_parallel_transfer_wave',
    'SELECT sum(length(l.payload)::bigint + r.payload) > 0 FROM heterogeneous_wave_hub h JOIN heterogeneous_wave_left l USING (k) JOIN heterogeneous_wave_right r USING (k) WHERE h.keep');
SET bloompg.enable = on;
SELECT sum(length(l.payload)::bigint + r.payload) > 0
    AS heterogeneous_wave_query_ok
FROM heterogeneous_wave_hub h
JOIN heterogeneous_wave_left l USING (k)
JOIN heterogeneous_wave_right r USING (k)
WHERE h.keep;
SELECT (
       SELECT count(*)
       FROM jsonb_array_elements(
           bloompg_last_profile()->'transfer'->'relations') AS relation
       WHERE relation->'materialization'->'wave'->>'kind' = 'parallel-append'
         AND relation->'materialization'->'wave'->>'layout' = 'heterogeneous'
   ) = 2 AS heterogeneous_wave_profile_ok;
RESET bloompg.transfer_parallel_min_rows;
RESET bloompg.transfer_workers;
SET bloompg.profile = off;
RESET bloompg.profile_log;

SELECT count(*) = 0 AS filter_contexts_released
FROM pg_backend_memory_contexts
WHERE name LIKE 'BloomPG%filter%';

SET max_parallel_workers_per_gather = 0;
SET bloompg.profile = on;
SET bloompg.profile_log = off;
SET bloompg.late_materialize = off;
SELECT count(*) > 0 AS correlated_projection_query_ok
FROM a JOIN c ON a.k = c.k
WHERE a.keep
  AND EXISTS (
      SELECT 1 FROM b
      WHERE b.k = a.k AND b.k2 = a.k2);
SELECT COALESCE((
    SELECT (relation->'projection'->>'columns')::integer = 3
       AND ARRAY(
           SELECT (attribute->>'attno')::integer
           FROM jsonb_array_elements(
               relation->'projection'->'attributes') AS attribute
           ORDER BY (attribute->>'attno')::integer
       ) = ARRAY[1, 2, 5]
    FROM jsonb_array_elements(
        bloompg_last_profile()->'transfer'->'relations') AS relation
    WHERE relation->>'alias' = 'a'
), false) AS correlated_projection_profile_ok;

-- Track an outer Var through more than one SubLink level.  The expected set
-- is semantic (join key, correlated key, local qual), not a physical-width
-- shortcut tied to a benchmark table.
SELECT count(*) > 0 AS nested_correlated_projection_query_ok
FROM a JOIN c ON a.k = c.k
WHERE a.keep
  AND EXISTS (
      SELECT 1 FROM b
      WHERE b.k = a.k
        AND EXISTS (
            SELECT 1 FROM c AS nested_c
            WHERE nested_c.k = b.k AND nested_c.k2 = a.k2));
SELECT COALESCE((
    SELECT ARRAY(
           SELECT (attribute->>'attno')::integer
           FROM jsonb_array_elements(
               relation->'projection'->'attributes') AS attribute
           ORDER BY (attribute->>'attno')::integer
       ) = ARRAY[1, 2, 5]
    FROM jsonb_array_elements(
        bloompg_last_profile()->'transfer'->'relations') AS relation
    WHERE relation->>'alias' = 'a'
), false) AS nested_correlated_projection_profile_ok;

-- Whole-row and system-column Vars deliberately keep the native scan tuple
-- shape.  Projection is an optimization and must fail closed for both.
SELECT count(a) > 0 AS whole_row_projection_query_ok
FROM a JOIN c ON a.k = c.k
WHERE a.keep;
SELECT COALESCE((
    SELECT (relation->'projection'->>'columns')::integer =
           (relation->'projection'->>'original_columns')::integer
    FROM jsonb_array_elements(
        bloompg_last_profile()->'transfer'->'relations') AS relation
    WHERE relation->>'alias' = 'a'
), false) AS whole_row_projection_fail_closed_ok;

SELECT count(a.ctid) > 0 AS system_column_projection_query_ok
FROM a JOIN c ON a.k = c.k
WHERE a.keep;
SELECT COALESCE((
    SELECT (relation->'projection'->>'columns')::integer =
           (relation->'projection'->>'original_columns')::integer
    FROM jsonb_array_elements(
        bloompg_last_profile()->'transfer'->'relations') AS relation
    WHERE relation->>'alias' = 'a'
), false) AS system_column_projection_fail_closed_ok;
SET bloompg.late_materialize = on;
SELECT count(*) > 0 AS profiled_query_ok
FROM a JOIN c ON a.k = c.k
WHERE a.keep;
SELECT bloompg_last_profile()->>'status' = 'complete'
   AND (bloompg_last_profile()->'planning'->>'transfer_ms')::numeric >= 0
   AND (bloompg_last_profile()->'planning'->>'statistics_ms')::numeric >= 0
   AND jsonb_typeof(bloompg_last_profile()->'transfer'->'steps') = 'array'
   AND jsonb_array_length(bloompg_last_profile()->'transfer'->'relations') >= 2
   AND (bloompg_last_profile()->'transfer'->'summary'->>'statistics_rows')::numeric >= 0
   AND (bloompg_last_profile()->'transfer'->'summary'->>'statistics_key_sets')::integer > 0
   AND (bloompg_last_profile()->'transfer'->'summary') ? 'precomputed_key_sets'
   AND (bloompg_last_profile()->'transfer'->'summary') ? 'scanned_key_sets'
   AND (bloompg_last_profile()->'transfer'->'summary') ? 'one_pass_bitmap_builds'
   AND (bloompg_last_profile()->'transfer'->'summary') ? 'filter_finalize_rescans'
   AND (bloompg_last_profile()->'transfer'->'summary'->>'parallel_waves')::integer
       <= (bloompg_last_profile()->'transfer'->'summary'->>'parallel_scans')::integer
   AND (bloompg_last_profile()->'transfer'->'relations'->0->'sampling')
       ? 'probe_filter_checks'
   AND (bloompg_last_profile()->'transfer'->'relations'->0) ? 'intermediate_sampling'
   AND (bloompg_last_profile()->'transfer'->'relations'->0->'materialization')
       ? 'one_pass_bitmap_builds'
   AND (bloompg_last_profile()->'transfer'->'relations'->0->'materialization')
       ? 'filter_finalize_rescans'
   AND bloompg_last_profile()->'executor' ? 'run_ms'
    AS structured_profile_ok;

CREATE TABLE intermediate_parent(k integer, keep boolean);
CREATE TABLE intermediate_child(k integer);
INSERT INTO intermediate_parent
SELECT g, g <= 10 FROM generate_series(1, 100) AS g;
INSERT INTO intermediate_child
SELECT 1 + g % 100 FROM generate_series(1, 1000) AS g;
ANALYZE intermediate_parent, intermediate_child;
SET bloompg.transfer_progress_metric = 'rows';
SELECT count(*) = 100 AS intermediate_sample_query_ok
FROM intermediate_parent AS p
JOIN intermediate_child AS c USING (k)
WHERE p.keep;
SELECT COALESCE((
    SELECT (relation->'intermediate_sampling'->>'calls')::integer > 0
       AND (relation->'intermediate_sampling'->>'last_fraction')::numeric = 1.0
    FROM jsonb_array_elements(
        bloompg_last_profile()->'transfer'->'relations') AS relation
    WHERE relation->>'alias' = 'p'
), false) AS intermediate_same_domain_ok;
RESET bloompg.transfer_progress_metric;
SET bloompg.profile = off;
RESET bloompg.profile_log;

DROP SCHEMA bloompg_regress CASCADE;
DROP EXTENSION bloompg;
RESET client_min_messages;
