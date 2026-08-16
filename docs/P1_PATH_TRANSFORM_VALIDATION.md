# P1 access-path preservation validation

Date: 2026-08-16
Target: BloomPG on PostgreSQL 18.4

## Root cause

The old P1 hook selected one cheapest unparameterized base path and placed a
BloomPG node around it. A parameterized path such as
`partsupp_supplier_idx(ps_suppkey, ps_partkey)` was therefore excluded before
PostgreSQL could compare it with the other access paths. The executor also
needed explicit `chgParam` forwarding because PostgreSQL's generic rescan code
does not propagate parameter changes through `CustomScanState.custom_ps`.

The first Filter-only refactor exposed a second, independent index issue in
TPC-H Q20. `partsupp_supplier_idx` was preserved, but the locally selective
`part` relation was a Memory replacement. P1 placed that column store inside a
parameterized subtree and reconstructed about 17.8 million slots instead of
using `part_pk_idx` for keyed lookup.

## Refactored boundary

P1 now has two explicit operations:

1. Memory, TID, and Empty are relation replacements. They may replace the
   native PathSet.
2. Filter is a PathSet transform:

   ```text
   Filter(PathSet) = { Filter(path) | path in PathSet }
   ```

`path_transform.c` wraps every eligible normal and partial path. It does not
choose SeqScan, IndexScan, IndexOnlyScan, BitmapHeapScan, a parallel path, or a
parameterized path. Each wrapper retains the native Path as its executable
child and propagates `param_info`/`required_outer`, `pathkeys`, target,
parallel properties, rows, disabled-node count, startup cost, and total cost.
Bloom selectivity and membership-test cost are applied per candidate before
the normal PostgreSQL path comparison.

A local-qual-only Memory/TID materialization is semantically equivalent to its
native parameterized paths. Those keyed alternatives now remain beside the
replacement, allowing P1 to choose between a sequential column-store read and
repeated index probes. If materialization consumed any incoming Bloom filter,
the native paths are not equivalent and are not restored.

At execution, a parameterized Filter scan forwards its `chgParam` set to the
native child and follows PostgreSQL's lazy-rescan convention. EXPLAIN reports
the wrapped access method, whether the child is parameterized, total rescans,
and parameter-change rescans.

## Correctness and path coverage

The fixed regression suite covers:

- SeqScan;
- ordinary IndexScan;
- IndexOnlyScan;
- parameterized IndexScan with changing outer values;
- BitmapHeapScan;
- parallel Filter paths and DSM transport;
- a local-only Memory replacement for which P1 selects an equivalent
  parameterized native index;
- result-bag equality with Bloom disabled;
- snapshot fallback, repeated rescan, cursor, spill, and parallel execution.

The profile assertion verifies that Filter input-path count equals wrapped-path
count and that parameterized paths are present. A separate counter verifies
that equivalent parameterized replacement paths survive P1. After stabilizing
the synthetic cost shapes, the full SQL regression passed ten consecutive
runs. The benchmark-runner unit suite passed 7/7 tests.

## TPC-H SF10 indexed validation

Protocol: one warmup, one measured run, no timeout, identical PostgreSQL
settings, complete result hashes, 128 PostgreSQL worker ceiling, `work_mem =
512MB`, and `hash_mem_multiplier = 2`.

| Query | Native PG | BloomPG | Speedup | Planning | Transfer | P1 | Final execution |
|---|---:|---:|---:|---:|---:|---:|---:|
| Q4 | 1202.952 ms | 1179.374 ms | 1.020x | 3.552 ms | 3.052 ms | 0.000 ms | 1174.841 ms |
| Q20 | 5062.724 ms | 4545.010 ms | 1.114x | 437.691 ms | 435.998 ms | 0.715 ms | 4105.544 ms |

Both result hashes match. Q4 correctly declines transfer. Q20's selected plan
contains:

```text
Custom Scan (BloomPG Filter)
  Wrapped Access Path: Index Scan
  Parameterized Child: true
  Rescans: 4054
  Parameter Change Rescans: 4054
  -> Index Scan using partsupp_supplier_idx on partsupp

-> Index Scan using part_pk_idx on part
```

The Q20 Filter transform observed six input paths and wrapped all six: one
SeqScan, five IndexScans, and three parameterized paths. It also retained one
equivalent parameterized path beside a Memory replacement. Column-store slot
reconstructions fell from 17,817,067 in the intermediate bad plan to 1 in the
final plan. The correlated lineitem index subplan returned to 247,287 searches,
the native-plan count.

Evidence: `benchmark_results/pg_round75_tpch_q04_q20_equivalent_parameterized_v7.jsonl`.
The rejected intermediate result is retained as
`benchmark_results/pg_round74_tpch_q04_q20_p1_path_transform_v6.jsonl`.

## JOB regression sample

The current `bloompg_job` database has no user indexes, so the new equivalent
parameterized-path branch is inactive there. This is a useful control showing
that the index refactor does not perturb an index-free workload.

| Query | Native PG | BloomPG | Speedup |
|---|---:|---:|---:|
| 01a | 190.409 ms | 62.734 ms | 3.035x |
| 05a | 71.428 ms | 70.521 ms | 1.013x |
| 05b | 68.586 ms | 71.773 ms | 0.956x |
| 08c | 1547.489 ms | 695.223 ms | 2.226x |
| 13c | 393.474 ms | 181.448 ms | 2.169x |
| 32a | 166.477 ms | 65.858 ms | 2.528x |

All result hashes match. Aggregate latency is 2437.863 ms native versus
1147.557 ms BloomPG, or 2.124x. The only regression is 05b at about 4.4%, below
the 10% regression gate. The previously completed 113-query JOB run remains
the broad workload characterization; this targeted run is the current-code
regression check.

Evidence: `benchmark_results/pg_round76_job_index_regression_p1_path_transform_v7.jsonl`.

## Remaining risks

- A Memory/TID materialization that consumed incoming Bloom filters cannot yet
  retain a plain native index path. Supporting that case requires either a
  keyed in-memory access path or a semantically equivalent filtered native
  alternative with retained filter lifetime.
- Filter wrappers conservatively advertise projection only. PostgreSQL may add
  Material when backward scan or mark/restore is required.
- A native path whose target omits a Bloom probe key causes the whole relation
  transform to fail closed to the untouched native PathSet.
- Partial paths are transformed only when all filter hash functions are
  parallel safe.
- Q20 transfer is now correct and bounded but still costs about 436 ms, mostly
  the exact scan of the unindexed `p_name LIKE 'forest%'` source.
- These are warm, single-user, one-measurement numbers; they establish the
  access-path regression result, not production variance or concurrency.
