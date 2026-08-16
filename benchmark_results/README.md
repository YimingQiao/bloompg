# Benchmark evidence

This directory tracks release benchmark records only. Each JSONL row contains
the query hash, effective PostgreSQL and BloomPG settings, warmup/measured wall
times, result-bag hashes, and the final BloomPG profile.

Development experiments remain local and are ignored by Git. Release files use
the stable naming convention:

- `release_pg18_job113_ndv.jsonl`
- `release_pg18_job113_rows.jsonl`
- `release_pg18_tpch_sf10_ndv.jsonl`

Generate records with `scripts/run_pg_benchmark.py`. A release run uses one
unreported same-backend warmup, one measured execution, no statement timeout,
identical PostgreSQL settings in both modes, and matching result hashes.

The release JOB files reuse a native side whose recorded database, indexes,
query text, settings, and hashes match the final run; all BloomPG sides are
fresh. The TPC-H release file runs both sides after `VACUUM (ANALYZE)` seals
the static loaded dataset. This visibility-map step is part of the physical
benchmark profile because it determines whether PostgreSQL can perform real
index-only scans.
