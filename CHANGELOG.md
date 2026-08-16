# Changelog

All notable changes to BloomPG are documented here.

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
