# Contributing to BloomPG

BloomPG is a PostgreSQL 18 extension with an unusual phase boundary: it safely
executes selected relation operators during planning, then asks PostgreSQL to
plan again. Changes therefore need semantic, planner, executor, and performance
validation.

## Development loop

Build against the exact PostgreSQL 18 installation you intend to test:

```bash
make clean PG_CONFIG=/path/to/pg18/bin/pg_config
make -j PG_CONFIG=/path/to/pg18/bin/pg_config
make install PG_CONFIG=/path/to/pg18/bin/pg_config
make installcheck PG_CONFIG=/path/to/pg18/bin/pg_config \
  PGHOST=/tmp PGPORT=55432
python3 -m unittest discover -s test/python -v
```

The Makefile rejects another PostgreSQL major version. A successful build is
not sufficient: private planner/executor structures can change between minor
releases, so run the regression suite on every supported PG18 minor.

## Where changes belong

- Query eligibility and P0/P1 orchestration: `planner.c`.
- Equality graph construction and legal transfer directions: `graph.c`.
- Fixed-point policy and filter/statistics scheduling: `transfer.c`.
- Plan-tree discovery/projection/replacement utilities: `transfer_plan.c`.
- Exact scans, row-to-column receivers, index-assisted reads, compaction, and
  parallel waves: the focused `transfer_*.c` modules.
- Post-P1 runtime artifact reclamation: `transfer_cleanup.c`.
- Relation replacements and post-transfer selectivity: `replan.c`.
- Filter-over-PathSet transformation: `path_transform.c`.
- Executor-facing plan construction, leaves, rescans, and DSM transport:
  `scan_plan.c` and the focused `scan_*.c` modules.
- Flat vectors, selection, and query-local memory accounting: `column_store.c`.
- Sampling is scheduling-only: `sampling.c`.

Do not expose transfer-private state in `transfer.h`; add a narrow contract to
`transfer_internal.h` only when two transfer implementation modules truly need
it. Keep extension-facing declarations in their existing public header.

## Correctness tests

Add a deterministic case to `test/sql/bloompg.sql` and its expected result for
every semantic change. Compare Bloom enabled/disabled as row bags. Include the
native fallback path when adding a new runtime object or query shape. At a
minimum consider NULLs, duplicates, self joins, outer/semi/anti direction,
prepared execution, stale snapshots, rescans, cursor direction, and parallel
workers.

Sampling estimates are never correctness evidence. A test that only checks an
`EXPLAIN` shape is not a substitute for result equivalence.

## Performance protocol

The benchmark runner records effective settings, alternates mode order, warms
each mode once in its own backend, measures the requested repetitions, and
hashes the complete result bag. Example:

```bash
python3 scripts/run_pg_benchmark.py \
  --queries-dir /path/to/tpch/queries --database tpch_sf10 \
  --sample-mode prepared \
  --sample-size 10000 \
  --parallel-workers 16 --transfer-workers 16 \
  --work-mem 512MB --hash-mem-multiplier 2 \
  --statement-timeout-ms 0 --timed-runs 1 \
  --output benchmark_results/my_change_tpch_sf10.jsonl
```

`load_tpch_sf10.sh` builds the indexes in `tpch_pg_indexes.sql` by default and
finishes with `VACUUM (ANALYZE)`. This seals the static dataset's visibility
map so both modes see the same real index-only paths. It is the full
canonical-query profile and avoids PostgreSQL parameterized full scans for
correlated TPC-H queries. Set `TPCH_CREATE_INDEXES=0` only for a separately
labelled heap-scan profile; do not combine its timings with the indexed
profile.

For an indexed JOB profile, apply `scripts/job_pg_indexes.sql`. It recreates
the schema primary-key paths plus ordinary indexes for every schema-level
foreign-key/join column; it contains no query-specific indexes.

Use `--recursive --queries-per-group N` for a deterministic, stratified CEB
sample. Run the full corpus before making a release-wide performance claim.
Report total wall-clock speedup and geometric mean together; the first shows
workload throughput and the second prevents a few long queries from hiding
widespread regressions. Always report mismatches and failed queries.

Before merging performance-sensitive work, inspect `bloompg_last_profile()`.
Attribute time to sampling, physical scan waves, column compaction, filter
finalization, statistics, P1, and final execution. A faster final executor does
not compensate for an unexplained transfer regression.
