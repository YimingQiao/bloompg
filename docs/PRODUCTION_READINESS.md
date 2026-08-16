# Production readiness

BloomPG is a functional PostgreSQL 18 research/analytical extension with strong
single-query JOB results. It is not yet a drop-in default for a multi-tenant
production cluster. The table below separates correctness safeguards that are
already implemented from operational evidence still required.

## Risk register

| Priority | Risk | Current behavior | Required before broad production use |
|---|---|---|---|
| P0 | Aggregate memory is not governed | `bloompg.materialization_memory` is a per-backend, per-table ceiling. The half-RAM single-query default can multiply across tables and sessions; `work_mem` also multiplies per node and worker. | Add a per-query total materialization budget/admission control; deploy today only with a concurrency-aware value from `bloompg_tune.py`. |
| P0 | Concurrent analytical queries are uncharacterized | Transfer workers share PostgreSQL's global worker pool; one query may request a large group. Correct fallback exists when fewer workers launch, but latency/fairness is not measured. | Run 1/2/4/8-client throughput, peak-RSS, worker-exhaustion, cancel, and mixed OLTP/OLAP tests. Set per-query worker ceilings. |
| P0 | Planning performs real query work | Exact scans, filter propagation, and column materialization happen inside the planner hook and hold the statement snapshot/locks. Monitoring that reports only executor time will be misleading. | Establish planning-time SLOs, cancellation tests, lock-duration tests, and end-to-end telemetry collection. |
| P1 | The index guard is heuristic | A large relation waits for an exact join-key set when a compatible leading B-tree path exists. If every candidate waits, the smallest one is scanned to preserve progress. TPC-H Q21 shows that this can be slower than native PostgreSQL when the needed join key is not a leading index column. | Benchmark the deployment corpus and disable BloomPG per role/session where the fallback is unfavorable; a future policy needs a robust benefit signal without turning the guard into an unsafe cost model. |
| P1 | Spill changes the execution format | In-memory chunks are columnar, but crossing the ceiling falls back to a row-form tuplestore and can consume temp I/O. Disk-full remains a query error. | Benchmark forced spill, set `temp_file_limit`, size temp storage, expose spill bytes/files, and prefer a columnar spill format later. |
| P1 | Leader-side parallel bottleneck | Workers scan in parallel, but Gather tuples are demultiplexed and appended to backend-local column stores by the leader. | Profile larger SF/worker counts; consider DSM-native column chunks or worker-local chunk/filter states. |
| P1 | Hook coexistence is only structurally chained | Planner, relation-path, and join-path hooks call the previous hook, but load order and assumptions of extensions such as TimescaleDB, pg_hint_plan, or columnar AMs are not covered. | Build an explicit compatibility matrix and regression cluster for supported extension combinations. |
| P1 | PG18 implementation coupling | The build is intentionally rejected for other majors. Planner/executor internals may still change in an 18.x update. | CI against every supported PG18 minor and pre-release; publish a tested-version matrix. |
| P1 | Prepared sample cache is operational state | Prepared CTID samples affect scheduling only and are correctness-safe when stale, but cache permissions, cleanup, corruption, relation churn, and storage growth need operations policy. | Add cache inventory/eviction tooling, corruption tests, metrics, and documented ownership/backup exclusions. |
| P1 | Long-lived/cached plan lifecycle needs stress | Runtime tokens are snapshot-tagged and stale plans fall back natively. The mechanism is covered functionally, not by long prepared-plan/DDL/churn stress. | Stress PREPARE, connection pools, DISCARD, DDL, VACUUM FULL, partition churn, aborts, and repeated plain EXPLAIN while checking memory. |
| P1 | Type/operator coverage is conservative | Strict hashjoinable equality and compatible hash support are handled; unsupported query shapes bypass safely. Extension-defined types, collations, partition AMs, and unusual operators are not comprehensively tested. | Differential fuzz across supported types/collations and document a tested SQL feature matrix. |
| P2 | Profiling/log volume | Structured profiling is query-local, but `profile_log` can emit large JSON records and detailed transfer logging can be noisy. | Default log collection off in high-QPS systems; add sampling/redaction/size policies and exporter integration. |
| P2 | Benchmark evidence is narrow | JOB and TPC-H SF10 are complete, but measurements are warm, one-user, one measured iteration on one machine; CEB characterization is incomplete. | Add variance, cold-cache, concurrency, TPC-DS, skew/update, and hardware-diverse CI/perf gates. |

## Already fail-closed

- Modifying statements/CTEs, row locks, row-level security, volatile
  expressions, lateral/table-sample scopes, and unsupported joins retain the
  native PostgreSQL plan.
- Formal filters come only from exact scans under the active snapshot; samples
  cannot remove rows.
- Optimizer-time failures roll back an internal subtransaction and replan
  natively, except cancellation, shutdown, and resource-failure classes that
  must remain visible to the client.
- Runtime materializations and filters are snapshot-tagged. A stale/missing
  token executes its native fallback.
- P1-unselected objects, executor-completed objects, and plain-EXPLAIN objects
  have explicit cleanup paths.

These properties protect result correctness. They do not solve resource
isolation, latency predictability, or coexistence with arbitrary extensions.

The resource warnings are consistent with PostgreSQL's own model: `work_mem`
can be consumed by several operations in one query, hash nodes multiply it by
`hash_mem_multiplier`, and parallel workers apply resource limits per process.
See the PostgreSQL 18
[resource-consumption documentation](https://www.postgresql.org/docs/18/runtime-config-resource.html).

## Recommended guarded deployment today

1. Use a dedicated analytical PostgreSQL 18 cluster or a tightly controlled
   analytical role, not all OLTP sessions.
2. Generate concurrency-aware settings with `scripts/bloompg_tune.py`; never
   use the half-RAM materialization ceiling unchanged for multiple sessions.
3. Cap `bloompg.transfer_workers`, PostgreSQL global workers, `work_mem`,
   `temp_file_limit`, and connection count as one resource model.
4. Enable `bloompg.profile` during canarying but keep per-query JSON logging off
   unless the log pipeline is sized for it.
5. Compare result hashes and p50/p95 end-to-end latency with Bloom disabled for
   the actual query corpus. Maintain a role/session kill switch via
   `bloompg.enable=off`.
6. Monitor planning time, temp bytes, backend RSS, worker availability,
   cancellation latency, sample-cache size, and native-fallback frequency.
7. Pin a tested PG18 minor and extension build; rerun regression and workload
   gates before upgrades.

## Release gates

A production release should require: zero result mismatches; clean sanitizer
and long abort/cancel runs; bounded peak memory under declared concurrency;
forced-spill success; no leaked tokens across PREPARE/DDL/EXPLAIN churn; a hook
compatibility matrix; full JOB/TPC-H/CEB reports; and multi-client throughput
that does not starve ordinary PostgreSQL work.
