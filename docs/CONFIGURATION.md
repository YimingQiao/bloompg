# Configuration

BloomPG settings are session-level unless noted otherwise. Load the module
before reading or changing them:

```sql
LOAD 'bloompg';
```

## Propagation policy

```sql
SET bloompg.transfer_progress_metric = 'ndv';  -- default
SET bloompg.transfer_progress_metric = 'rows'; -- compatibility mode
```

`ndv` changes only convergence detection for eligible equality-key transfers.
Scheduling still uses estimated/live row counts. For a canonical, same-type
integral equality class, BloomPG records the exact distinct-key count and a
version per relation. Equal exact NDV proves that a transferred subset did not
change the key domain, so the redundant transfer can be skipped without
storing a second key set.

`rows` retains the original row-cardinality and filter-lineage propagation.
Composite keys, cross-type joins, approximate Bloom filters, protected join
sides, and unsupported equality classes use this normal path in either mode.

## Main settings

| Setting | Default | Purpose |
|---|---:|---|
| `bloompg.enable` | `on` | Enable planner integration. |
| `bloompg.sample_mode` | `prepared` | Use persistent `prepared` or query-local `instant` samples. |
| `bloompg.sample_size` | `10000` | Target rows per base-relation sample. |
| `bloompg.sample_seed` | `2` | Deterministic prepared-reservoir seed. |
| `bloompg.sample_rate` | `0.01` | Chunk fraction sampled from an already materialized relation; probes use the same vector filter interface as compaction without modifying the stored selection. |
| `bloompg.sample_cache_dir` | `auto` | Prepared cache directory; `auto` uses `$PGDATA/bloompg_samples`. Superuser-only. |
| `bloompg.sample_memory_cache` | `on` | Retain prepared samples in backend memory; cached CTIDs and hydrated tuples share a limit of half `materialization_memory`. |
| `bloompg.excitation_threshold` | `1.0` | Re-excite below this fraction of the preceding cardinality. |
| `bloompg.local_first_fraction` | `0.001` | Candidate threshold for evaluating a selective local qual first. |
| `bloompg.bits_per_key` | `24` | Bloom-filter bit budget per estimated key. |
| `bloompg.max_filter_bytes` | `64MB` | Per-filter memory ceiling. |
| `bloompg.transfer_progress_metric` | `ndv` | Equality-transfer convergence signal: `rows` or `ndv`. |
| `bloompg.materialization_memory` | up to `2GB` | Total memory budget for one query's optimizer-time materializations and parallel DSM copies. The default is one eighth of detected host/cgroup memory, capped at 2GB. |
| `bloompg.transfer_workers` | `4` | Maximum workers for one eligible transfer scan; `0` is serial. |
| `bloompg.parallel_filter_build` | `on` | Build private outgoing filters during direct parallel transfer scans, then merge them in the leader; `off` retains leader-side construction. |
| `bloompg.transfer_parallel_min_rows` | `100000` | Minimum estimated input rows for parallel transfer scanning. |
| `bloompg.replan_after_transfer` | `on` | Run the second PostgreSQL planning pass. |

When this budget cannot hold the next vector allocation, BloomPG abandons its
optimizer work and executes the original PostgreSQL plan. It never silently
turns the materialization into a disk spill. Use `bloompg_tune.py` to size the
query-wide budget for dedicated analytical deployments and concurrency.

## Reoptimization

BloomPG supplies reduced base-relation cardinalities and access paths to the
second PostgreSQL planning pass. For materialized base relations, it also exposes
collected single-column join-key NDVs and NULL fractions through PostgreSQL's
`get_relation_stats_hook`. These query-local statistics do not modify
`pg_statistic`; PostgreSQL estimates joins and chooses the execution plan.

Statistics become visible after the corresponding base path is replaced, so
original scan predicates are not estimated against already filtered rows.
Original MCV lists and histograms are not attached to the updated column
statistics. Composite-key NDVs are retained internally but are not passed as
single-column NDVs: PostgreSQL 18's column-statistics hook cannot express them,
and its extended statistics do not provide general multicolumn join estimation.
Columns without collected single-column statistics keep PostgreSQL's normal
statistics lookup. Tables with only pending filters likewise retain that lookup.

## Worker-local filter construction

Direct parallel transfer scans build one private filter per distinct outgoing
key in each participant, including the leader when it scans rows. Bloom filters
use the same global size and hash parameters and are merged by word-wise OR.
Integer bitmap candidates retain their signed, word-aligned ranges and are
merged with matching offsets. Duplicate destination keys reuse the merged filter.
Surviving row vectors still pass through Gather for materialization; only filter
construction moves to the scan participants. Filter summaries use bounded 64KiB
word payloads rather than one large message.

Private allocations are reserved against `materialization_memory` before the
scan starts, including leader participation and temporary bitmap growth. If
that reservation fails or a build hash function is not parallel-safe, the scan
retains leader-side construction. Each worker's bitmap candidate is capped at
8MB (and by `max_filter_bytes`); an abandoned or globally unsuitable bitmap uses
the existing materialization-to-Bloom fallback. Parallel Append prefetch and
in-memory compaction retain their existing construction paths.

Per-relation materialization profiles expose `worker_filter_parts` (one private
filter per participating process), `worker_filter_bytes` (summary payload bytes),
`worker_filter_build_ms` (sum of participant build times), and
`worker_filter_merge_ms` (leader merge time). Participant time overlaps scanning
and must not be added to wall-clock query time. `filter_prepare_ms` counts the
leader's inline construction or merge work; worker construction remains inside
the parallel scan's elapsed time.

## Index-assisted materialization

`bloompg.filter_cost_rows_per_ms` enables DuckDB Bloom's scheduling score:
`2 * estimated_output_rows + coefficient * estimated_local_predicate_ms`.
The default is `1000` rows/ms; `0` retains output-only scheduling. This is a
conversion between time and tuple-cost units, not a measured PostgreSQL scan
throughput.

When enabled, local conjuncts are timed once on the existing sample, in their
original order with short-circuit evaluation. Cached cumulative pass masks are
intersected with incoming-filter survivors to estimate each predicate's input.
New filters update these estimates without re-evaluating local predicates.
Sample acquisition and I/O are excluded from predicate timing; per-row executor
overhead remains included. Already materialized or prefetched rows incur no
additional local-predicate charge. Lightweight runtime-filter CPU is uncharged,
as in DuckDB Bloom. Missing sample costs contribute zero. The estimate assumes
incoming filters precede local predicates; it does not model disk-read cost.

Selective index access keeps its existing priority. Within either priority
tier the score replaces output size; eligible bitmap access still breaks ties.
Transfer-step profiles expose `selection_filter_ms` and `selection_score`.

For controlled comparisons, `scripts/run_pg_benchmark.py` accepts
`--filter-cost-rows-per-ms` and `--effective-io-concurrency`. Omit either option
to inherit the corresponding server setting. The I/O setting applies equally
to native PostgreSQL and BloomPG; the result metadata records both settings,
as well as PostgreSQL's `io_method` and `io_workers`.
Baseline reuse requires matching SQL and native settings; changing I/O or
parallelism settings forces a fresh native run. Changing only BloomPG's
predicate-cost coefficient can reuse a successful native baseline.

Count I/O workers as part of the CPU budget. With PostgreSQL 18's
`io_method=worker`, they compete with query execution when the server is pinned
to one CPU. In warm OS-cache experiments, `effective_io_concurrency=0` can
avoid that overhead: it requests synchronous reads, not zero reads. With
multiple CPUs, asynchronous bitmap heap reads can instead be faster. Set CPU
affinity on the postmaster before it starts so its children inherit the same
allocation; limiting parallel-query workers alone does not constrain the CPU
budget. Recheck the I/O setting for cold storage and different hardware.

| Setting | Default | Purpose |
|---|---:|---|
| `bloompg.index_transfer` | `on` | Use native indexed access for exact incoming join keys on a compatible B-tree index. |
| `bloompg.index_priority_max_fraction` | `0.01` | Additional individual index-filter selectivity limit for scheduling priority. |
| `bloompg.index_transfer_max_fraction` | `0.07` | Maximum individual index-filter selectivity for indexed access, independent of worker count; broader or unestimated conditions remain residual filters. |

Parallel bitmap heap scans use PostgreSQL's native executor: one participant
builds the bitmap, then the leader and available workers retrieve matching
heap pages. No PostgreSQL backend patch is required.

With index transfer enabled, SSPT first considers active, unmaterialized tables
with an eligible incoming index filter whose individual sampled selectivity is
at or below the priority threshold. Among these it selects the
lowest scheduling score. If none qualify, it selects the lowest score
among all active tables. Within the same priority tier, equal scores
prefer a table with eligible bitmap access, even if its index selectivity
exceeds the priority threshold; remaining ties use table RTI. For the selected
table, each compatible exact index filter must separately meet the access
threshold before contributing a bitmap. A small joint selectivity does not
justify probing a broad individual index. Excluded conditions are still checked
on retrieved rows. Without an eligible filter, BloomPG retains its
scan path. Unknown individual selectivity does not grant index access or priority,
and broader conditions remain residual filters. Priority never uses the
combined selectivity of unrelated filters.

By default, each eligible key set is passed as one complete array scan key to PostgreSQL's
index API. There is no auxiliary SQL query, optimizer invocation, manual batch
size, or key-count truncation. Current support is for exact, one-column
integral/date key sets with matching canonical equality and a compatible
nonpartial B-tree index. Approximate Bloom filters remain residual filters.
PostgreSQL allocation and array representation limits apply.

Indexed access uses a fixed per-condition selectivity ceiling (7% by default).
The ceiling is identical for serial and parallel access: it is not divided by
worker count, and there is no key-count or NDV exception. Conditions that exceed
the ceiling remain residual filters; if none qualify, BloomPG retains its scan
path. This is an empirical access rule, not a prediction of elapsed time.

When transfer workers, query safety, and the memory budget allow parallel
execution, BloomPG can construct a native indexed scan under its existing Gather
and materialization operators. The worker count determines execution parallelism,
not the selectivity ceiling. Nonredundant eligible conditions remain available
for bitmap intersection.

Eligible index conditions construct TID bitmaps,
intersected when multiple nonredundant conditions are available. Serial execution
uses bitmap heap retrieval; parallel execution uses Parallel Bitmap Heap Scan.
The choice does not depend on correlation, heap page count, or worker count.
PostgreSQL owns shared scan state, work distribution, visibility checks,
lossy-page rechecks, and worker cleanup. No available workers is handled by
leader execution. Exact-key arrays are sent as executor parameters, avoiding
repeated serialization in different copies of the index condition.

Parallel eligibility also requires the estimated number of touched heap pages
to meet PostgreSQL's `min_parallel_table_scan_size`. Smaller indexed scans and
single-core configurations retain serial bitmap access. Runtime filters are
evaluated before local predicates and payload projection. The profile reports
`parallel-bitmap` or serial `bitmap-index-probe`, including bitmap page counters.
Query workers and asynchronous I/O workers are separate: benchmark comparisons
must fix both CPU affinity and PostgreSQL's I/O configuration.

## P1 and materialization

| Setting | Default | Purpose |
|---|---:|---|
| `bloompg.late_materialize` | `on` | Allow survivor-CTID materialization for wide relations. |
| `bloompg.late_materialize_min_width` | `128B` | Projected-width threshold for TID mode. |

P1 exposes serial and parallel paths for exact in-memory materializations up
to the configured worker ceiling, along with logically equivalent parameterized
index paths. Serial memory scans receive no additional disabled-node penalty.
During reoptimization, an inner Nested Loop join whose outer path contains an
Anti Join prefers equivalent parameterized index access over a full rescan of
an SSPT memory table, including a rescan through Materialize. This guards
against underestimated Anti Join output without penalizing memory scans in
other joins. The rule applies to ordinary base tables with all index parameters
supplied by the outer path; without an equivalent index path, normal costing
is unchanged. After successful transfer and reoptimization, BloomPG executes
the replacement plan without comparing its estimated total cost against the
original plan. If transfer fails or produces no replacement, the native plan
remains available. The former `bloompg.index_guard_min_rows` setting has been
removed.

`bloompg.parallel_tuple_cost_factor` scales PostgreSQL's current
`parallel_tuple_cost` only while planning the reduced inputs (P1). The default
factor is `0.1`; `1` retains PostgreSQL's original cost. Scan and index costs,
native planning, and transfer planning are unchanged. The original value is
restored after P1, including on errors; the setting does not force parallel
execution or relax parallel-safety checks. It affects all Gather and Gather
Merge costing within P1, so tune it against the workload rather than a single
query.

## Diagnostics

| Setting | Default | Purpose |
|---|---:|---|
| `bloompg.profile` | `off` | Collect the last query's structured profile. |
| `bloompg.profile_log` | `on` | Write completed profiles to the PostgreSQL log. |
| `bloompg.log_transfer_steps` | `off` | Log detailed transfer and shared-memory events. |

```sql
SET bloompg.profile = on;
SET bloompg.profile_log = off;

SELECT count(*) FROM fact JOIN dimension USING (key) WHERE dimension.keep;
SELECT jsonb_pretty(bloompg_last_profile());
SELECT bloompg_last_trace();
```

Prepared samples affect scheduling and estimates only. Every row-eliminating
filter is built from an exact scan under the active statement snapshot.

Index-access selectivity checks reuse per-filter results on the query-local
sample. The cache is keyed by the published filter token and bound columns, so
scheduling and scan selection avoid probing unchanged filters again. Profiles
expose `index_filter_probes` and `index_filter_cache_hits` in each relation's
`sampling` object.
