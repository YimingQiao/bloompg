# Changelog

All notable changes to BloomPG are documented here.

## 0.1.1 - 2026-08-16

- Stop an all-waiting index guard from performing an unbounded serial heap
  scan during planning; retain the already-built native PostgreSQL plan when
  no exact-key or parallel transfer path can make progress.
- Let PostgreSQL choose Index, Index Only, Bitmap, or parallel access first
  for exact-key transfer queries, with BloomPG's direct bitmap probe retained
  as the fallback implementation.
- Project correlated SubLink inputs by following outer Vars across arbitrary
  nesting levels. Whole-row and system-column references remain fail-closed.
- Add structured native-fallback diagnostics and regression coverage for
  guard stalemates and correlated projection.
- Add a 0.1.0-to-0.1.1 extension upgrade path, PGXN metadata, a security
  policy, and an automated upgrade smoke test.
- Validate the release against complete indexed JOB 113 and TPC-H SF10 gates.

## 0.1.0 - 2026-08-16

- Initial PostgreSQL 18 release.
- Adaptive exact Bloom/bitmap transfer with P0/P1 replanning.
- Columnar Memory, TID, Filter, Empty, and native-fallback relation paths.
- Index-aware delayed materialization and exact-key B-tree access.
- Switchable `rows` / `ndv` transfer progress metric; `ndv` uses a
  containment-safe size-only equality-domain summary.
- Structured optimizer/executor profiling and reproducible JOB/TPC-H runner.
- Indexed TPC-H loading seals the static dataset with `VACUUM (ANALYZE)` so
  native and BloomPG index-only paths share the same visibility-map state.
