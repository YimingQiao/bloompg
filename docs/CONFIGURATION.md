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
| `bloompg.sample_rate` | `0.01` | Chunk fraction sampled from an already materialized relation. |
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

| Setting | Default | Purpose |
|---|---:|---|
| `bloompg.index_transfer` | `off` | Use native bitmap access for selective exact incoming join keys on a compatible B-tree index. |
| `bloompg.index_transfer_max_fraction` | `0.10` | Maximum sampled selectivity of each index condition for bitmap access, evaluated separately from residual filters. |
| `bloompg.index_priority_max_fraction` | `0.01` | Maximum individual index-filter selectivity for scheduling priority; the index must also qualify for bitmap access. |
| `bloompg.index_guard_min_rows` | `100000` | Existing P1 protection against repeated large materialization rescans when an equivalent parameterized index path exists; does not change transfer scheduling. |

With index transfer enabled, SSPT first considers active, unmaterialized tables
with an eligible incoming index filter whose individual sampled selectivity is
at or below both thresholds. Among these it selects the
smallest estimated output. If none qualify, it selects the smallest estimated
output among all active tables. Within the same priority tier, equal outputs
prefer a table with eligible bitmap access, even if its index selectivity
exceeds the priority threshold; remaining ties use table RTI. For the selected
table, a compatible exact index filter with estimated selectivity at or below
the access threshold drives a native B-tree bitmap scan, even if it did not
qualify for scheduling priority. Without an eligible filter,
BloomPG retains its scan path. Unknown individual selectivity does not qualify;
it is not replaced by the combined selectivity of other filters.

Each eligible key set is passed as one complete array scan key to PostgreSQL's
index API. There is no auxiliary SQL/ANY expression, optimizer invocation,
manual batch size, key-count threshold, or ordinary Index Scan fallback.
Nonredundant eligible sources contribute TID bitmaps that are intersected before
heap access. PostgreSQL manages the bitmap using `work_mem`; all local predicates
and incoming filters are rechecked, including on lossy pages. Current support
is for exact, one-column integral/date key sets with matching canonical equality
and a compatible nonpartial B-tree index. Approximate Bloom filters remain
residual filters. PostgreSQL allocation and array representation limits apply.

## P1 and materialization

| Setting | Default | Purpose |
|---|---:|---|
| `bloompg.late_materialize` | `on` | Allow survivor-CTID materialization for wide relations. |
| `bloompg.late_materialize_min_width` | `128B` | Projected-width threshold for TID mode. |

P1 exposes serial and parallel paths for exact in-memory materializations up
to the configured worker ceiling. PostgreSQL's normal cost model chooses
between them. A narrow safety comparison retains the native plan only when P1
selects a more expensive parameterized-index Nested Loop shape.

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
