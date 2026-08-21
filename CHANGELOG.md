# Changelog

All notable changes to BloomPG are documented here.

## 0.1.2 - 2026-08-17

- Parallelize eligible transfer scans with relation-sized worker groups and
  preserve relation-local parallelism when an unrelated SubLink restricts the
  outer query.
- Use exact P1 inputs from 10,000 rows to expose parallel hash alternatives,
  avoiding serial join cliffs after successful transfer.
- Columnize scan output in workers, send vector packets through Gather, and
  bulk-import fixed-width and variable-width columns in the leader.
- Use the same vector packets for shared Parallel Append waves, including
  relations with different projections, instead of reconstructing rows in the
  leader.
- Build outgoing filters directly from arriving worker vectors, eliminating a
  second column-store pass on parallel materializations.
- Enforce one query-wide materialization budget across column stores, parallel
  packet buffers, and P1 DSM copies; if it cannot be reserved, retain the
  native PostgreSQL plan instead of spilling.
- Default that budget to one eighth of detected host/cgroup memory, capped at
  2GB for the intended single-query analytical workload.
- Bound backend-level prepared CTID and hydrated tuple caches and validate
  every serialized vector, selection, filter, and by-reference datum before
  worker access.
- Preserve selective index-only paths, use exact-key PostgreSQL index/bitmap
  plans where appropriate, and retain the native plan for parameterized index
  joins and unsafe index-guard stalemates.
- Keep parallel replacement and filter state available below Gather, while
  separating Gather-shared cursors from worker-replicated nested-loop cursors
  so each rescan restarts at the correct executor lifecycle boundary.
- Remove the emergency P1 nested-loop suppression switch now that parallel
  rescans follow PostgreSQL's CustomScan DSM lifecycle directly.
- Initialize NULL bitmap-probe Datums before the null-aware fast path, avoiding
  undefined reads under compiler instrumentation.
- Prune materialized projections through nested correlated SubLinks and retain
  fail-closed behavior for whole-row and system-column references.
- Add assertion-enabled regression coverage for vector packets, toasted and
  NULL values, parallel safety, index guards, and correlated projection.

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
