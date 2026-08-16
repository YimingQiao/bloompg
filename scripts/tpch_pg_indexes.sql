-- PostgreSQL physical design for the indexed TPC-H profile.
--
-- DuckDB decorrelates several TPC-H subqueries into scans/joins. PostgreSQL 18
-- can otherwise choose a parameterized full scan for Q2, Q17, Q20, Q21, or
-- Q22. These key indexes keep the canonical SQL executable and also provide a
-- stronger, production-realistic native baseline for BloomPG.

CREATE UNIQUE INDEX IF NOT EXISTS region_pk_idx
    ON region (r_regionkey);
CREATE UNIQUE INDEX IF NOT EXISTS nation_pk_idx
    ON nation (n_nationkey);
CREATE UNIQUE INDEX IF NOT EXISTS supplier_pk_idx
    ON supplier (s_suppkey);
CREATE UNIQUE INDEX IF NOT EXISTS customer_pk_idx
    ON customer (c_custkey);
CREATE UNIQUE INDEX IF NOT EXISTS part_pk_idx
    ON part (p_partkey);
CREATE UNIQUE INDEX IF NOT EXISTS partsupp_pk_idx
    ON partsupp (ps_partkey, ps_suppkey);
CREATE INDEX IF NOT EXISTS partsupp_supplier_idx
    ON partsupp (ps_suppkey, ps_partkey);
CREATE UNIQUE INDEX IF NOT EXISTS orders_pk_idx
    ON orders (o_orderkey);
CREATE INDEX IF NOT EXISTS orders_customer_idx
    ON orders (o_custkey, o_orderkey);
CREATE UNIQUE INDEX IF NOT EXISTS lineitem_pk_idx
    ON lineitem (l_orderkey, l_linenumber);
CREATE INDEX IF NOT EXISTS lineitem_part_supplier_shipdate_idx
    ON lineitem (l_partkey, l_suppkey, l_shipdate)
    INCLUDE (l_quantity, l_extendedprice);
CREATE INDEX IF NOT EXISTS lineitem_order_supplier_idx
    ON lineitem (l_orderkey, l_suppkey);

-- COPY leaves the visibility map empty.  Seal the static analytical dataset
-- so PostgreSQL's indexed baseline and BloomPG's P1 plans get real index-only
-- scans, then collect the statistics used by both modes.
VACUUM (ANALYZE);
