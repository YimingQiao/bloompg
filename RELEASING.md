# Release checklist

BloomPG currently targets PostgreSQL 18. A release is tied to the tested PG18
minor and is not assumed compatible with another PostgreSQL major.

## Before a release

1. Update `default_version` in `bloompg.control`, the extension SQL filename,
   `bloompg_version()`, the README version badge, and `CHANGELOG.md` together.
2. Build from a clean tree against PostgreSQL 18 and run:

   ```bash
   make clean PG_CONFIG=/path/to/pg18/bin/pg_config
   make -j PG_CONFIG=/path/to/pg18/bin/pg_config
   make install PG_CONFIG=/path/to/pg18/bin/pg_config
   make installcheck PG_CONFIG=/path/to/pg18/bin/pg_config \
     PGHOST=/tmp PGPORT=55432
   make pythoncheck
   ```

3. Run the full indexed JOB 113 and TPC-H SF10 22-query gates with prepared
   sampling, `transfer_progress_metric=ndv`, one warmup plus one measured run,
   and no timeout. Seal a freshly loaded static TPC-H database with `VACUUM
   (ANALYZE)` before either mode so visibility-map state is identical. Every
   result hash must match native PostgreSQL.
4. Run the full JOB `rows` control when transfer semantics or the NDV domain
   implementation changes. Record both results under `benchmark_results/`.
5. Review `docs/PRODUCTION_READINESS.md`; do not describe the extension as a
   general multi-tenant production default until every stated release gate is
   satisfied.
6. Check repository hygiene before committing:

   ```bash
   git status --short --ignored
   git ls-files | grep -E '\.(o|bc|so|pyc)$' && exit 1 || true
   git grep -n -E '(ghp_|github_pat_|password[[:space:]]*=|secret[[:space:]]*=)'
   ```

7. Push the release commit, wait for CI, then create an immutable annotated
   tag. Never move an existing release tag.

## Benchmark protocol

The checked-in runner records all effective settings and hashes the complete
result bag. Native and Bloom modes must use the same database, indexes,
PostgreSQL settings, and query text. Optimizer-time transfer is planning work,
so benchmark wall time must include planning.
