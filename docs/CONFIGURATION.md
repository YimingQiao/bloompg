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
| `bloompg.materialization_memory` | up to `512MB` | Total memory budget for one query's optimizer-time materializations and parallel DSM copies. The default is capped at one eighth of the detected host/cgroup memory. |
| `bloompg.transfer_workers` | `4` | Maximum workers for one eligible transfer scan; `0` is serial. |
| `bloompg.transfer_parallel_min_rows` | `100000` | Minimum estimated input rows for parallel transfer scanning. |
| `bloompg.replan_after_transfer` | `on` | Run the second PostgreSQL planning pass. |

When this budget cannot hold the next vector allocation, BloomPG abandons its
optimizer work and executes the original PostgreSQL plan. It never silently
turns the materialization into a disk spill. Use `bloompg_tune.py` to size the
query-wide budget for dedicated analytical deployments and concurrency.

## Index-assisted materialization

| Setting | Default | Purpose |
|---|---:|---|
| `bloompg.index_transfer` | `on` | Let exact incoming join keys drive a compatible B-tree path. |
| `bloompg.index_transfer_max_fraction` | `0.15` | Maximum sampled survivor fraction for choosing that path. |
| `bloompg.index_transfer_batch_keys` | `65536` | Exact keys in one direct B-tree probe batch. |
| `bloompg.index_transfer_max_keys` | `100000` | Maximum exact distinct-key population eligible for index transfer. |
| `bloompg.index_transfer_planner_max_keys` | `8192` | Maximum keys expanded into one private PostgreSQL access plan. |
| `bloompg.index_guard_min_rows` | `100000` | Large indexed relations wait for an exact incoming join-key set. |

The index guard delays only relations whose base cardinality reaches the
threshold and whose join key has a compatible B-tree index. If every active
candidate is waiting, BloomPG selects the smallest current candidate. It may
scan that candidate when its current row estimate is no larger than
`index_transfer_max_keys`, because its distinct key count cannot be larger, or
when the scan has an eligible parallel transfer path. A larger serial
candidate with no ready exact-key index path ends transfer and retains the
native PostgreSQL preview plan. The structured profile reports
`native_fallback_reason = "index-guard-stalemate"` for this case.

## P1 and materialization

| Setting | Default | Purpose |
|---|---:|---|
| `bloompg.p1_parallel_bias` | `on` | Let exact post-transfer cardinalities favor parallel P1 paths. |
| `bloompg.p1_parallel_min_rows` | `10000` | Minimum exact rows before applying the parallel cost bias. |
| `bloompg.p1_disable_nestloop` | `off` | Emergency guard that suppresses P1 nested loops. |
| `bloompg.late_materialize` | `on` | Allow survivor-CTID materialization for wide relations. |
| `bloompg.late_materialize_min_width` | `128B` | Projected-width threshold for TID mode. |

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
