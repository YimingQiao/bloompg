# BloomPG agent guide

BloomPG targets PostgreSQL 18. Keep user-facing and code terminology as
`Bloom` / `bloompg`. The project license names only Yiming Qiao.

## Start here

Read these files before changing execution semantics:

1. `docs/ARCHITECTURE.md` for module ownership and data flow.
2. `docs/DESIGN.md` for algorithm and correctness invariants.
3. `docs/PRODUCTION_READINESS.md` for known operational limits.
4. `test/sql/bloompg.sql` for the supported semantic surface.

The public implementation headers are under `src/include/bloompg/`.
`transfer_internal.h` is deliberately private to the transfer subsystem.

## Non-negotiable invariants

- A sample may schedule work or estimate cardinality; it must never eliminate
  a result row.
- Only an exact source execution under the active statement snapshot may
  populate a formal filter.
- Each base relation is physically materialized at most once per query.
- After the first read, filter build and compaction operate on flat column
  chunks; do not introduce a second row-form in-memory copy.
- Physical parallel waves may overlap heap reads, but logical propagation must
  preserve the smallest-active fixed point.
- P1 must see sealed survivor counts/statistics, and must remain free to choose
  PostgreSQL's native upper operators.
- A stale runtime token or snapshot mismatch must execute the native fallback.
- Unsupported syntax must fail closed to a native PostgreSQL plan.

## Verification

Always select PostgreSQL 18 explicitly:

```bash
make clean PG_CONFIG=/path/to/pg18/bin/pg_config
make -j PG_CONFIG=/path/to/pg18/bin/pg_config
make install PG_CONFIG=/path/to/pg18/bin/pg_config
make installcheck PG_CONFIG=/path/to/pg18/bin/pg_config \
  PGHOST=/tmp PGPORT=55432
python3 -m unittest discover -s test/python -v
```

For performance claims, use `scripts/run_pg_benchmark.py`. Both modes must use
identical PostgreSQL settings, one warmup plus one measured execution, no
statement timeout, and matching result hashes. Never compare runs that omit
planning time: optimizer-time transfer is part of BloomPG query latency.

Keep generated objects, logs, sample caches, and ad-hoc results out of source.
Checked-in benchmark JSONL is evidence and should have a descriptive, immutable
name rather than overwriting an earlier round.
