# BloomPG design

## Contract

BloomPG preserves the defining execution model of Bloom for DuckDB while
respecting PostgreSQL's planner, executor, MVCC, and extension boundaries:

1. PostgreSQL produces an executable candidate plan (`P0`).
2. BloomPG builds a binding-aware equality graph from the rewritten query and
   augments it with physical join clauses (needed for pulled-up sublinks).
3. Prepared or instant samples estimate the initially selective relations.
4. An adaptive excitation loop fully executes selected relation subplans under
   the active statement snapshot.
5. Each exact source scan builds Bloom/bitmap filters, propagates lineage, and
   re-estimates affected samples before choosing the next source.
6. Exact reduced relations are sealed in flat column vectors. BloomPG collects
   survivor width, NULL, and composite join-key NDV statistics from already
   built exact filters where possible, scanning only uncovered final vectors.
7. Exact reduced relations, their statistics, and direct filters enter a
   query-local replacement registry.
8. PostgreSQL plans again (`P1`) using those paths and post-transfer statistics.
9. PostgreSQL executes the resulting native join/aggregate plan with BloomPG
   CustomScan leaves.

Sampling never removes a formal query row. Only a filter populated by an exact
source execution under the current snapshot may prune a destination.

No stage depends on PostgreSQL's native `HashJoin` build table. In particular,
P0 may use a hash join to expose equality clauses, but BloomPG separately scans
the chosen source and constructs its own reusable filter before P1.

## Phase boundary

```text
rewritten Query
  -> standard_planner P0 (parallel disabled, executable relation inputs)
  -> query/physical equality graph and protection directions
  -> sample cardinalities
  -> optimizer-time exact scan + filter transfer + feedback loop
  -> vector selection seal + post-transfer statistics
  -> replacement registry (column store token, rows, width, join-key stats)
  -> standard_planner P1 (normal PostgreSQL settings, parallel allowed)
  -> PostgreSQL executor
       -> native joins, aggregates, sorts, Gather, indexes
       -> BloomPG Memory / TID / Filter / Empty leaves
       -> Memory lazily fills one reusable virtual TupleTableSlot per request
```

P0 temporarily discourages nested loops so a parameterized inner path is not
mistaken for an independently executable table operator. P1 restores every
user setting and remains free to choose hash, merge, or nested-loop joins.

Optimizer-time execution runs inside an internal subtransaction. Query cancel,
shutdown, out-of-memory, and internal-error conditions propagate. Other
execution errors roll back the nested work, release its runtime objects, and
return a fresh native plan.

## Adaptive transfer

Every relation starts with a base cardinality and a lineage containing itself.
A relation is excited when its current estimate falls below the configured
fraction of the preceding cardinality (exact zero is always excited). The
smallest active relation is executed first.

There is one scheduling exception. Before its first materialization, a
relation whose base cardinality exceeds `bloompg.index_guard_min_rows` and
whose join probe key is the first column of a compatible B-tree index is
guarded until an exact incoming key set makes an index path available. An
index-ready relation continues to wait while an active, smaller-base indexed
source has not sent its keys, which orients indexed chains from smaller tables
toward larger tables. All unguarded relations retain the normal
smallest-current-cardinality order. If every active relation is guarded, that
same order chooses one relation to scan and breaks the wait; the guard never
turns into a second cost model or a new correctness boundary.

`bloompg.index_guard_min_rows` classifies relation size only.
`bloompg.index_transfer_max_keys` is independent: it limits the number of
exact distinct keys accepted by an optimizer-time B-tree probe after the key
set exists.

With `bloompg.transfer_progress_metric = 'ndv'`, canonical same-type integral
equalities also have a size-only domain summary. Setting it to `rows` retains
the original row-cardinality/lineage propagation for direct A/B comparison.
BloomPG does not retain a second copy of the key set: it reuses the exact NDV
from the dense bitmap popcount, stores a domain version, and records which
version each relation has received. A relation that consumed the current
version can only emit a subset of that exact key domain. Equal NDV therefore
proves that the set is unchanged and its outgoing transfer can be skipped;
smaller NDV advances the version. A source that has not received the current
version remains on the normal edge path.

This deduction is deliberately fail-closed. It applies only to unprotected
single-column inner equalities with an exact bitmap, and only when each
relation contributes one attribute to the equality class. Composite,
cross-type, collated, approximate-Bloom, protected, or multi-attribute shapes
retain normal propagation. Thus NDV is never used as a probabilistic guess
that two unrelated sets are equal; equality is inferred only under the
containment invariant.

One exact scan can feed several outgoing edges. Transfers with the same source
row set, build attributes, operators, collations, and orientation share one
physical Bloom filter. Composite edges hash all key columns in a stable order.
Before first materialization, a persisted or instant CTID sample is evaluated
with the original scan quals and every formal incoming filter. Once a relation
has an exact column-store result, feedback switches to a stratified 1% sample
of that current store. Newly received filters are evaluated on the same
selected chunks, and the incremental `after / before` fraction is multiplied
by the store's exact live-row count. This matches DuckDB's intermediate CDC
sampling domain and prevents base-sample error from masquerading as a new
reduction. That feedback decides whether the destination should execute next
or wait for final filter application.

Dense bitmaps are used only for a single canonical, same-type integral equality
whose observed range fits the memory/density limits. All other supported keys
use the operator family's compatible build/probe hash support functions; this
also handles cross-type equality and collations without byte-level assumptions.

An eligible first exact scan is executed by PostgreSQL processes under a
`Gather`. `bloompg.transfer_workers` is an upper bound; BloomPG scales the
request to approximately one worker per 100,000 estimated input rows so a
million-row JOB table does not pay for a 128-process group intended for an
SF10 fact table. The request is also bounded by PostgreSQL's per-Gather and
global worker pools.

Two excited relations may share one physical wave. Their current cardinalities
must preserve the smallest-active order: unrelated peers are admitted only
when they are within 2x of the coordinator. Type/typmod/collation-permutable
target lists use a compact common reordered schema. Small heterogeneous peers
(at most 250,000 estimated rows apiece) use a tagged tuple with two disjoint
nullable regions, each retaining its relation's native types. Larger
heterogeneous peers stay on the ordered one-source path to avoid widening a
large Gather queue. In either layout BloomPG builds one
`Gather -> Parallel Append` plan and the leader demultiplexes rows directly into
two separate column stores. This avoids a second worker-group startup and avoids
serializing rows through `bytea` or a tuplestore.

Propagation remains ordered: any filter built by the coordinator is applied by
a selection-only compaction of the prefetched peer before that peer is allowed
to propagate. Thus concurrency changes physical heap scheduling, not the
DuckDB Bloom excitation/fixed-point semantics. BloomPG deliberately avoids two
independent Gather groups because their worker startup and tuple queues contend
at the leader.

## Relation outcomes

Each affected table operator finishes in one form:

- `EMPTY`: a full formal execution observed exactly zero rows. The node still
  carries a native child for snapshot mismatch or stale prepared plans.
- `MEMORY`: a projected exact result is retained as 2048-row flat column
  vectors. Fixed-width values use native-width arrays, by-reference values use
  offsets into chunk-local heaps, validity uses bitmaps, and survivor
  compaction rewrites only the selection vector.
- `TID`: a wide base relation retains survivor CTIDs; heap rows are fetched
  under the executor snapshot only when P1 consumes them.
- `FILTERED`: every native normal and partial path is transformed into an
  equivalent Bloom-filtered path. Seq, Index, IndexOnly, Bitmap, parallel,
  and parameterized candidates retain their access semantics; PostgreSQL,
  not BloomPG, performs the final comparison.
- `UNCHANGED`: no legal and useful transfer replaced the relation.

The P1 path hook installs normal and partial CustomPaths. A Memory/TID result
that was produced from local quals only is also equivalent to the relation's
native parameterized paths, so those keyed alternatives remain available next
to the replacement. This prevents a small nested-loop outer from repeatedly
rescanning a column store. If materialization consumed any incoming Bloom
filter, native paths are not equivalent and the replacement remains strict.
Memory/TID tuples and direct-filter metadata/bitsets are copied into dynamic
shared memory. Workers claim materialized rows atomically, while a FILTER
worker applies the same Bloom or bitmap before returning rows to the native
parallel plan.

## Snapshot and prepared-plan safety

Runtime objects are tagged with a signature of the active PostgreSQL snapshot.
CustomScan startup looks up both token and signature. If either is absent, the
node initializes its native fallback child and does no pruning. Consequently a
cached prepared plan cannot reuse old tuples, CTIDs, filters, or an old empty
result after command visibility changes.

Prepared samples have a different contract: a deterministic CTID reservoir is
keyed by database, relation storage identity, size, and seed and may outlive a
query. The current snapshot fetches any still-visible sample TIDs. This sample
can change scheduling or estimates but cannot authorize row elimination.

Executor end callbacks release selected runtime objects. Unselected P1
replacements are released immediately after planning. A pre-commit callback
also closes objects made only for plain `EXPLAIN`, where no executor callback
exists. Columnar stores use `bloompg.materialization_memory`; after that
budget is exhausted, their row-form tuplestore fallback spills according to
PostgreSQL's `work_mem` policy.

## P1 statistics and lazy row boundary

The first optimizer pass sees the original base-table statistics. Transfer can
change a join key's population by orders of magnitude, so reusing the original
`pg_statistic` selectivity during P1 would apply the old reduction a second
time. Every exact Memory/TID materialization therefore carries statistics
sealed after its final selection:

- exact survivor and NULL counts;
- actual projected width;
- HLL distinct estimates for the complete composite equality key shared with
  each materialized peer.

For a canonical integral key, the adaptive bitmap candidate is populated while
the first flat-vector pass discovers its range. If its final range and density
are suitable, the formal bitmap adopts that candidate without rereading the
column store. The bitmap popcount then gives exact non-NULL distinct count and
NULL count for the same final population, so P1 statistics reuse it directly.
Only a range/density fallback or an uncovered composite key needs another
selected-vector pass.

P1 installs exact base rows and widths with the replacement CustomPath. For an
inner edge whose endpoints both have exact materializations, the join hook uses
the post-transfer non-NULL fractions and the larger composite-key NDV as the
selectivity denominator. Multiple cycle-closing relation pairs are treated as
correlated rather than blindly multiplied. This correction happens inside the
second PostgreSQL planning pass; the emergency
`bloompg.p1_disable_nestloop` guard is off by default.

P1 never reads materialized tuples, so no column-to-row conversion is required
before optimization. The final executor pulls rows from Memory CustomScan. The
scan follows the chunk selection, points a reusable virtual slot at the
requested column values, and immediately returns it to the native PostgreSQL
operator. A physical row copy is created only if an upper PostgreSQL node
chooses to materialize one.

## Semantic boundaries

- Only strict, hashjoinable `Var = Var` clauses become transfer edges.
- Inner and semi joins allow useful positive membership transfer in both
  directions.
- The preserved side of left/right outer and anti joins is protected; both
  sides of full joins are protected.
- Composite keys are transferred together; an `OR` expression never becomes
  a composite membership test.
- Set operations and recursive scopes are graph boundaries.
- Lateral/parameterized table operators are not detached.
- Modifying statements/CTEs, row locking, row-level security, table sampling,
  volatile expressions, and unsupported relation kinds bypass Bloom.
- Unsupported subquery or partition shapes remain native rather than being
  partially rewritten.

## Diagnostics

`EXPLAIN (ANALYZE)` identifies each CustomScan and reports tokens, row counts,
snapshot reuse/fallback, direct-filter counts, DSM use, and rejected rows.
`bloompg_last_trace()` exposes the last graph, sample estimates, excitation
steps, filter kind, cache hit/miss, replacement form, and P1 status for the
current backend. `bloompg.log_transfer_steps` adds detailed server-log events.
Filter scans also report their wrapped access path, parameterized-child status,
rescans, and parameter-change rescans.

With `bloompg.profile = on`, executor hooks attach wall-clock execution data to
the planner-side record and `bloompg_last_profile()` returns JSON. The record
contains structured P0/graph/transfer/statistics/P1 timings, the ordered
transfer edges, and per-relation base/intermediate sampling, exact-scan, prefilter,
column-compaction, spill, memory, survivor, width, NULL, and composite-key NDV
statistics. `planning.statistics_ms` isolates the statistics-finalization
cost. In-memory compaction and bitmap rebuilds use zero-copy column-chunk
views; `slot_compactions` is therefore expected to stay zero unless the
materialization crossed its memory budget and fell back to a tuplestore.
Before first materialization, each sample workspace retains an append-only
formal-filter survivor mask. New feedback probes only the newly appended
filters over prior survivors; the profile exposes rows examined, filter checks,
filters applied, and unchanged-state cache hits.
Parallel diagnostics distinguish logical relation scans from physical worker
waves. Both members of a shared wave expose the same wave ID and wall clock,
plus the compact/heterogeneous layout, while the summary counts that clock only
once; this prevents overlapping Parallel Append work from being misreported as
additive transfer time. Bitmap finalize-rescan rows and precomputed/scanned P1
key sets make avoidable second passes explicit.
`planning.path_transform` records input/wrapped/retained Filter paths by access
family, parameterized and partial counts, plus equivalent parameterized paths
retained beside Memory/TID replacements. `bloompg.profile_log` writes the same
completed JSON object to the server log.

## Functional checklist

- [x] Table-operator discovery and binding lineage
- [x] Query and physical join equality discovery
- [x] Composite edges and direction protection
- [x] Prepared and instant samples
- [x] Adaptive excitation with sample feedback
- [x] PostgreSQL operator-family Bloom hashing
- [x] Safe exact dense integral bitmaps
- [x] Filter build cache and lineage propagation
- [x] Memory, TID, direct-filter, empty, and unchanged outcomes
- [x] Second planner pass with replacement cardinalities
- [x] Post-transfer composite-key statistics consumed by P1
- [x] Flat-vector materialization and lazy virtual-slot row boundary
- [x] Parameters, self joins, materialized CTEs, subqueries, and partitions
- [x] Rescan/backward execution for nested loops, merge joins, and cursors
- [x] Parallel Memory/TID transfer and worker-side direct filtering
- [x] Size-adaptive PostgreSQL-process transfer scans and compact/heterogeneous same-wave Parallel Append
- [x] Snapshot-stale prepared-plan fallback
- [x] EXPLAIN and query-level diagnostics
- [x] Fixed semantic regression and 60-query deterministic differential matrix
- [x] Full current-version PostgreSQL 18 JOB performance characterization
- [x] Representative PostgreSQL 18 TPC-H SF10 performance characterization
- [ ] Full CEB performance characterization
