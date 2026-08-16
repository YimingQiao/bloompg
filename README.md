# BloomPG

[![PostgreSQL 18](https://img.shields.io/badge/PostgreSQL-18-336791.svg)](https://www.postgresql.org/)
[![Version 0.1.0](https://img.shields.io/badge/version-0.1.0-blue.svg)](CHANGELOG.md)
[![CI](https://github.com/YimingQiao/bloompg/actions/workflows/ci.yml/badge.svg)](https://github.com/YimingQiao/bloompg/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

BloomPG is an experimental PostgreSQL 18 implementation of
[Bloom](https://github.com/YimingQiao/bloom). It executes selected relation
inputs during planning, propagates exact bitmap or Bloom filters through the
join graph, and gives the reduced relations back to PostgreSQL for a second
planning pass and normal execution.

The implementation is conservative: samples influence scheduling only, exact
scans own row elimination, and unsupported or unsafe query shapes fall back to
PostgreSQL's native plan.

## How it works

1. PostgreSQL produces an executable analysis plan, P0.
2. BloomPG samples base inputs and builds a binding-aware equality graph.
3. A smallest-active fixed point executes selected relation inputs exactly,
   transfers filters, and updates survivor estimates.
4. Reduced relations become columnar Memory, late-materialized TID, direct
   Filter, Empty, or unchanged native paths.
5. PostgreSQL plans again, P1, using exact survivor statistics and then owns
   joins, aggregates, sorts, parallelism, and final execution.

For a large relation with a compatible join-key B-tree index, BloomPG delays
materialization until an exact incoming key set can use that index. If every
active relation is waiting, the smallest current candidate is scanned to keep
the fixed point moving. This is one heuristic guard, not a cost model.

Equality propagation has a switchable convergence signal:

    SET bloompg.transfer_progress_metric = 'ndv';  -- default
    SET bloompg.transfer_progress_metric = 'rows'; -- original behavior

`ndv` stores only an exact distinct-key count and a version for eligible
single-column integral equality classes. Under the containment invariant,
equal NDV proves that the key domain did not change, so BloomPG can suppress
the redundant transfer without retaining another key set. Scheduling still
uses row counts. Composite, cross-type, approximate, protected, and unsupported
classes keep the normal transfer path.

See [the design document](docs/DESIGN.md) for the correctness argument and
[the architecture map](docs/ARCHITECTURE.md) for module ownership.

## Compatibility

- PostgreSQL 18 only; developed and tested with PostgreSQL 18.4.
- Read-only queries with hashjoinable strict equality joins.
- Inner, outer, semi, anti, composite, cross-type, self, prepared-parameter,
  CTE, subquery, view, and supported partitioned-relation shapes.
- Linux is the tested platform.

Modifying statements, row locking, row-level security, recursive or modifying
CTEs, volatile expressions, and unsafe scopes bypass BloomPG. An
optimizer-time execution error is rolled back in an internal subtransaction
and replanned natively.

BloomPG is currently an analytical/research extension, not a general
multi-tenant production default. Review
[production readiness](docs/PRODUCTION_READINESS.md) before deployment.

## Build and load

PostgreSQL 18 server headers and PGXS are required.

    make PG_CONFIG=/path/to/postgresql-18/bin/pg_config
    make PG_CONFIG=/path/to/postgresql-18/bin/pg_config install
    make PG_CONFIG=/path/to/postgresql-18/bin/pg_config installcheck
    make pythoncheck

Create the extension in each database:

    CREATE EXTENSION bloompg;

Planner hooks are process-local. Load BloomPG in every backend, normally with:

    shared_preload_libraries = 'bloompg'

Restart PostgreSQL after changing that setting. For development, load one
session directly:

    LOAD 'bloompg';
    SELECT bloompg_version();
    SHOW bloompg.enable;
    SHOW bloompg.transfer_progress_metric;

## Benchmarks

The release measurements below use PostgreSQL 18.4 on the same 512 GiB,
192-thread analytical host. Each mode gets one unreported same-backend warmup
and one measured run. Planning time is included, there is no statement
timeout, PostgreSQL settings are identical, and the complete result bag is
hashed.

| Workload | Progress | Native PG | BloomPG | Total speedup | Geometric mean | Faster | Correct |
|---|---|---:|---:|---:|---:|---:|---:|
| Indexed JOB, 113 queries | `ndv` | 265.838 s | 61.622 s | 4.314x | 1.648x | 68/113 | 113/113 |
| Indexed JOB, 113 queries | `rows` | 265.838 s | 63.536 s | 4.184x | 1.572x | 67/113 | 113/113 |
| Indexed TPC-H SF10, 22 queries | `ndv` | 146.612 s | 155.708 s | 0.942x | 1.020x | 9/22 | 22/22 |

On JOB, switching from `rows` to `ndv` reduced total BloomPG time by 3.0%, cut
logical transfer steps from 5,305 to 2,158, and suppressed 3,120 redundant
filters. Size-only domain bookkeeping took 0.329 ms across all 113 queries. A
speedup above 1 means BloomPG was faster than native PostgreSQL; short native
queries can regress because sampling and two planning passes are fixed
overhead.

TPC-H total time is dominated by Q21 (3.091 s native versus 18.015 s
BloomPG). Its large `lineitem` input cannot use the available composite index
from the incoming join key. When every indexed candidate waits, the configured
guard deliberately scans the smallest candidate to make progress; the release
does not hide that known fallback cost with a query-specific exception.

The checked-in machine-readable evidence is:

- [JOB with NDV](benchmark_results/release_pg18_job113_ndv.jsonl)
- [JOB rows control](benchmark_results/release_pg18_job113_rows.jsonl)
- [TPC-H SF10 with NDV](benchmark_results/release_pg18_tpch_sf10_ndv.jsonl)

The JOB native timings are reused from an earlier pass with the same recorded
database, indexes, query text, PostgreSQL settings, and hashes; every JOB
BloomPG query was rerun with the release build. TPC-H runs both sides from the
same freshly vacuumed static dataset so index-only access has identical
visibility-map state.

### Running the benchmark

Load JOB or TPC-H, apply the matching indexes in
[scripts/job_pg_indexes.sql](scripts/job_pg_indexes.sql) or
[scripts/tpch_pg_indexes.sql](scripts/tpch_pg_indexes.sql), then run:

    TPCH_DUCKDB=/path/to/tpch_sf10.duckdb scripts/load_tpch_sf10.sh

    python3 scripts/run_pg_benchmark.py \
      --queries-dir /path/to/queries \
      --database benchmark_db --host /tmp --port 5432 \
      --pattern '.*' --timed-runs 1 --statement-timeout-ms 0 \
      --capture-profile --index-transfer \
      --transfer-progress-metric ndv \
      --output benchmark_results/release.jsonl

The JSONL records query/result hashes, warmup and measured times, effective
server/extension settings, database and index metadata, failures, and the
structured BloomPG profile. Summarize a run with:

    python3 scripts/summarize_benchmark.py benchmark_results/release.jsonl

## Configuration

The most important controls are:

    SET bloompg.enable = on;
    SET bloompg.sample_mode = 'prepared';         -- or 'instant'
    SET bloompg.transfer_progress_metric = 'ndv'; -- or 'rows'
    SET bloompg.index_transfer = on;
    SET bloompg.index_guard_min_rows = 100000;

The exact-key index path is bounded separately:

    SET bloompg.index_transfer_max_fraction = 0.15;
    SET bloompg.index_transfer_max_keys = 100000;

`index_transfer_max_keys` is the largest exact distinct-key set accepted for
optimizer-time B-tree probing; it is not a table-size estimate.

See [configuration](docs/CONFIGURATION.md) for every GUC and its default. For a
read-only hardware recommendation:

    python3 scripts/bloompg_tune.py

The tool does not change PostgreSQL settings. Both `work_mem` and
`bloompg.materialization_memory` multiply with analytical concurrency, so
review its output before applying it.

## Diagnostics

    SET bloompg.profile = on;
    SET bloompg.profile_log = off;

    SELECT count(*)
    FROM fact JOIN dimension USING (key)
    WHERE dimension.keep;

    SELECT jsonb_pretty(bloompg_last_profile());
    SELECT bloompg_last_trace();

The profile separates P0, transfer, post-transfer statistics, P1, and executor
time. It also reports relation outcomes, worker waves, exact-key index probes,
equality-domain observations/no-ops/suppressions, and fallback decisions.

## Testing and development

`make installcheck` runs fixed semantic cases and a deterministic differential
matrix with BloomPG enabled and disabled. It covers both progress metrics,
equality cycles and fail-closed boundaries, prepared plans, fallback,
replacement forms, join algorithms, cursors, and serial/parallel paths.

Before changing planner or executor behavior, read
[CONTRIBUTING.md](CONTRIBUTING.md), [AGENTS.md](AGENTS.md), and
[RELEASING.md](RELEASING.md).

## License

BloomPG is available under the [MIT License](LICENSE).
