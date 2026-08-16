# Code architecture

This document maps the algorithm in `DESIGN.md` onto source ownership. The
design goal is that policy, physical execution, PostgreSQL integration, and
runtime storage can evolve independently without hiding the P0/transfer/P1
phase boundary.

## Request path

```text
planner.c
  P0 -> graph.c -> transfer.c fixed point
                       |-> transfer_domain.c (exact NDV/version deduplication)
                       |-> sampling.c (estimates only)
                       |-> transfer_plan.c (plan-tree manipulation)
                       |-> transfer_materialize.c
                       |      -> filter.c + column_store.c + runtime.c
                       |      -> PostgreSQL Gather / Parallel Append
                       `-> statistics.c
       -> replan.c relation replacements + path_transform.c Filter(PathSet) -> P1
       -> transfer_cleanup.c drops P1-unselected artifacts
       -> scan_plan.c + scan_*.c expose selected leaves to the normal executor
       -> profile.c seals end-to-end telemetry
```

## Module ownership

| Module | Owns | Must not own |
|---|---|---|
| `bloompg.c` | `_PG_init`, GUC definitions, feature registration | transfer policy |
| `planner.c` | eligibility, hook chaining, P0/transfer/P1 transaction boundary and fallback | filter representation |
| `graph.c` | binding-aware equality graph and protected join directions | execution or sampling |
| `transfer.c` | excitation state machine, formal transfer scheduling, final compaction/statistics/replacements/profile assembly | raw plan-tree recursion or worker-plan construction |
| `transfer_domain.c` | exact integral equality-class NDV/version summaries and containment-safe no-op suppression | approximate set comparison or transfer scheduling |
| `transfer_plan.c` | plan discovery, subquery mapping, projection pruning, pointer replacement | executing a plan |
| `transfer_materialize.c` | exact source execution, slot receivers, row-to-column conversion, prefilter order, Gather/Parallel Append waves | deciding the fixed-point order |
| `transfer_cleanup.c` | tracing P1 and releasing unselected runtime tokens | creating replacements |
| `sampling.c` | prepared/instant base samples and intermediate column samples | formal row elimination |
| `filter.c` | compatible hashing, Bloom and exact bitmap operations | join direction policy |
| `column_store.c` | flat chunks, validity, varlen heaps, selection, lazy slots, spill | planner decisions |
| `statistics.c` | sealed survivor/null/NDV statistics | PostgreSQL path injection |
| `replan.c` | P1 Memory/TID/Empty relation replacements, equivalent parameterized alternatives, and corrected join selectivity | Filter PathSet transformation or materialization execution |
| `path_transform.c` | `Filter(PathSet)`: wrap every native normal/partial path while preserving parameterization, ordering, parallel, target, and cost properties | choosing a native access method or relation replacement |
| `scan_plan.c`, `scan_*.c` | BloomPG scan-plan construction, executor lifecycle, fallback, DSM transport, rescan, and EXPLAIN | transfer scheduling or P1 path policy |
| `runtime.c` | snapshot-tagged query-local token registry | long-lived global cache policy |
| `profile.c` | planner/executor profile lifetime and JSON | optimization decisions |

`transfer_internal.h` is the only shared private transfer contract. It carries
relation/pending state plus narrow cross-module functions. New fields should
describe fixed-point or profiling state; physical receiver structs remain
local to `transfer_materialize.c`.

## Data ownership and lifetime

P0 and the equality graph live in the planner context. Accepted runtime
materializations must outlive the optimizer's internal subtransaction, so
their column stores are allocated in `TopTransactionContext` and registered
under the planner's parent `ResourceOwner`. Tokens bind each object to the
active snapshot signature.

The first exact base read projects required columns and appends surviving slots
directly into 2048-row flat chunks. Chunks own fixed-width arrays, validity
bitmaps, and chunk-local varlen heaps. Compaction updates selections without
forming rows. P1 sees only metadata and tokens. At executor pull time a Memory
scan fills a reusable virtual slot; a row-form spill exists only after the
configured ceiling is crossed.

P1 may reject a speculative replacement. `transfer_cleanup.c` walks the final
plan and immediately releases any token that is no longer reachable. Executor
end and transaction callbacks cover selected paths, errors, and plain
`EXPLAIN`.

## Parallelism boundary

PostgreSQL extensions cannot create arbitrary executor threads inside a
backend. BloomPG builds detached parallel-safe scan plans and lets PostgreSQL
launch worker processes. A leader receiver converts worker tuples into the
query-local column store. Compatible pairs share a `Gather -> Parallel Append`
wave; logical propagation remains serial and ordered after the scan wave.

The current performance ceiling is therefore the leader-side Gather tuple
queue and row-to-column receiver. Moving column chunks themselves through DSM
would be a distinct future design, not a local optimization.

## Change rules

1. Keep public headers independent and self-contained.
2. Prefer a narrow cross-module function over reaching into another module's
   private receiver or executor state.
3. Any new fast path needs a native fallback and snapshot-mismatch test.
4. Profile a new phase explicitly; do not fold avoidable work into generic
   transfer time.
5. Preserve result-bag equivalence before evaluating speed.
