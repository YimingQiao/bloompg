# BloomPG

[![PostgreSQL 18](https://img.shields.io/badge/PostgreSQL-18-336791.svg)](https://www.postgresql.org/)
[![Version 0.1.2](https://img.shields.io/badge/version-0.1.2-blue.svg)](CHANGELOG.md)
[![CI](https://github.com/YimingQiao/bloompg/actions/workflows/ci.yml/badge.svg)](https://github.com/YimingQiao/bloompg/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

BloomPG accelerates complex analytical joins in PostgreSQL 18. It propagates
Bloom or exact bitmap filters across equality joins, materializes reduced
inputs, and lets PostgreSQL plan and execute the smaller join problem.

It is most useful when a selective table can eliminate a large fraction of one
or more joined tables. Existing SQL does not need to change, and unsupported
queries continue to use PostgreSQL's native planner.

## Results

PostgreSQL 18.4, indexed datasets, one warmup plus one measured run, planning
included, and complete result bags checked against native PostgreSQL:

| Workload | Mode | Native PG | BloomPG | Total speedup | Correct |
|---|---|---:|---:|---:|---:|
| JOB, 113 queries | `ndv` | 264.413 s | 62.654 s | **4.220x** | 113/113 |
| JOB, 113 queries | `rows` | 264.413 s | 63.256 s | **4.180x** | 113/113 |
| TPC-H SF10, 22 queries | `ndv` | 146.893 s | 140.728 s | **1.044x** | 22/22 |

## Install

BloomPG currently requires PostgreSQL 18 server headers and PGXS.

```bash
make PG_CONFIG=/path/to/postgresql-18/bin/pg_config
make PG_CONFIG=/path/to/postgresql-18/bin/pg_config install
```

Load the module in every backend, normally through `postgresql.conf`:

```conf
shared_preload_libraries = 'bloompg'
```

Restart PostgreSQL, then create the extension in each database:

```sql
CREATE EXTENSION bloompg;
SELECT bloompg_version();
```

To upgrade from 0.1.0 or 0.1.1 after installing the new files:

```sql
ALTER EXTENSION bloompg UPDATE TO '0.1.2';
```

## Use

BloomPG is enabled by default after it is loaded. Run analytical queries as
usual:

```sql
SELECT count(*)
FROM fact
JOIN dimension USING (dimension_id)
WHERE dimension.region = 'APAC';
```

The main controls are:

```sql
SET bloompg.enable = on;
SET bloompg.transfer_progress_metric = 'ndv'; -- default; use 'rows' for compatibility
SET bloompg.index_transfer = on;
SET bloompg.transfer_workers = 8;
```

`ndv` tracks only exact equality-key cardinality and is the recommended
default. `rows` keeps the original row-count propagation algorithm. See
[configuration](docs/CONFIGURATION.md) for all settings and memory guidance.

For query-level diagnostics:

```sql
SET bloompg.profile = on;
SET bloompg.profile_log = off;

-- Run the query, then inspect the latest profile.
SELECT jsonb_pretty(bloompg_last_profile());
SELECT bloompg_last_trace();
```

## Scope

- PostgreSQL 18 only; developed and tested with PostgreSQL 18.4 on Linux.
- Read-only queries with strict, hashjoinable equality joins.
- Inner, outer, semi, anti, composite, self, prepared-parameter, CTE,
  subquery, view, and supported partitioned-relation shapes.
- Modifying statements, row locking, row-level security, volatile expressions,
  and unsupported scopes bypass BloomPG.

BloomPG performs real scans during planning. Its query-wide materialization
budget defaults to at most 512MB and falls back to the native plan when full;
size it for expected analytical concurrency. BloomPG is intended for
controlled analytical workloads, not as an unreviewed default for a
multi-tenant OLTP cluster.

## Verify a build

```bash
make PG_CONFIG=/path/to/postgresql-18/bin/pg_config installcheck \
  PGHOST=/tmp PGPORT=5432
make pythoncheck PG_CONFIG=/path/to/postgresql-18/bin/pg_config
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and performance-test
guidelines. Report security issues privately as described in
[SECURITY.md](SECURITY.md).

## References

- Yifei Yang, Hangdong Zhao, Xiangyao Yu, and Paraschos Koutris,
  [*Predicate Transfer: Efficient Pre-Filtering on Multi-Join Queries*](https://arxiv.org/abs/2307.15255),
  CIDR 2024.
- Junyi Zhao, Kai Su, Yifei Yang, Xiangyao Yu, Paraschos Koutris, and Huanchen
  Zhang, [*Debunking the Myth of Join Ordering: Toward Robust SQL Analytics*](https://arxiv.org/abs/2502.15181),
  SIGMOD 2025.
- Yiming Qiao, Peter Boncz, and Huanchen Zhang,
  [*Robust Predicate Transfer with Dynamic Execution*](https://duckdb.org/library/robust-predicate-transfer-vldb/),
  PVLDB 2026.
- [Bloom reference implementation for DuckDB](https://github.com/YimingQiao/bloom).

## License

BloomPG is available under the [MIT License](LICENSE).
