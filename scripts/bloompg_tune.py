#!/usr/bin/env python3
"""Recommend PostgreSQL 18 and BloomPG settings for analytical workloads.

The command is intentionally read-only by default.  It detects host and
cgroup resources, accounts for expected analytical concurrency, and prints a
configuration fragment.  Use --output only after reviewing the suggestions.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


KIB = 1024
MIB = 1024 * KIB
GIB = 1024 * MIB


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--memory-gib",
        type=float,
        help="Usable memory in GiB; defaults to the smaller host/cgroup limit",
    )
    parser.add_argument(
        "--cpus",
        type=int,
        help="Usable logical CPUs; defaults to the process CPU affinity",
    )
    parser.add_argument(
        "--analytical-concurrency",
        type=int,
        default=1,
        help="Maximum simultaneously active analytical queries (default: 1)",
    )
    parser.add_argument(
        "--active-memory-operators",
        type=int,
        default=8,
        help="Conservative sort/hash operators per active query (default: 8)",
    )
    parser.add_argument(
        "--storage",
        choices=("ssd", "hdd"),
        default="ssd",
        help="Primary PostgreSQL storage class (default: ssd)",
    )
    parser.add_argument(
        "--format",
        choices=("text", "conf", "json"),
        default="text",
        help="Output format (default: text)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Write recommendations to this file after review",
    )
    return parser.parse_args()


def read_positive_integer(path: Path) -> int | None:
    try:
        value = path.read_text(encoding="ascii").strip()
    except OSError:
        return None
    if not value.isdigit():
        return None
    parsed = int(value)
    return parsed if parsed > 0 else None


def host_memory_bytes() -> int:
    try:
        for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
            if line.startswith("MemTotal:"):
                return int(line.split()[1]) * KIB
    except (OSError, ValueError, IndexError):
        pass
    pages = os.sysconf("SC_PHYS_PAGES")
    page_size = os.sysconf("SC_PAGE_SIZE")
    return int(pages) * int(page_size)


def detected_memory_bytes() -> tuple[int, str]:
    host = host_memory_bytes()
    candidates: list[tuple[int, str]] = [(host, "host")]
    for path in (
        Path("/sys/fs/cgroup/memory.max"),
        Path("/sys/fs/cgroup/memory/memory.limit_in_bytes"),
    ):
        limit = read_positive_integer(path)
        # Some cgroup v1 installations expose an enormous sentinel rather
        # than the word "max".  Ignore limits larger than physical memory.
        if limit is not None and limit <= host:
            candidates.append((limit, f"cgroup:{path}"))
    return min(candidates, key=lambda item: item[0])


def detected_cpus() -> int:
    try:
        return max(len(os.sched_getaffinity(0)), 1)
    except AttributeError:
        return max(os.cpu_count() or 1, 1)


def floor_to(value: int, quantum: int) -> int:
    """Round down without letting the quantum exceed a small target."""
    rounded = (value // quantum) * quantum
    return rounded if rounded > 0 else value


def clamp(value: int, lower: int, upper: int) -> int:
    return min(max(value, lower), upper)


def pg_size(value: int) -> str:
    if value % GIB == 0:
        return f"{value // GIB}GB"
    if value % MIB == 0:
        return f"{value // MIB}MB"
    return f"{value // KIB}kB"


def recommend(
    memory: int,
    cpus: int,
    concurrency: int,
    active_operators: int,
    storage: str,
) -> dict:
    shared_buffers = floor_to(memory // 8, GIB)
    effective_cache_size = floor_to(memory * 3 // 4, 8 * GIB)

    # Reserve at most a quarter of RAM for concurrently active PG executor
    # nodes.  work_mem is a per-node allowance, so divide before applying the
    # analytical cap used for this profile.
    executor_pool = memory // 4
    work_mem = executor_pool // concurrency // active_operators
    work_mem = floor_to(clamp(work_mem, 64 * MIB, 512 * MIB), 16 * MIB)

    maintenance_work_mem = floor_to(
        clamp(memory // 16, 512 * MIB, 4 * GIB), 256 * MIB
    )

    # BloomPG enforces this once across every materialization and parallel DSM
    # copy owned by a query.  Concurrent analytical backends share a 50% pool.
    materialization_budget = memory // 2 // concurrency
    materialization_quantum = GIB if materialization_budget >= GIB else 256 * MIB
    materialization_memory = floor_to(materialization_budget, materialization_quantum)

    # A transfer scan is a real PostgreSQL Gather and therefore consumes the
    # ordinary parallel-worker pool.  Allocate roughly two thirds of the CPU
    # share of one analytical query, rounded down to a four-process group, and
    # preserve capacity for all declared concurrent queries.  BloomPG treats
    # this as a ceiling and automatically shrinks small scans to about one
    # process per 100k estimated input rows.
    cpus_per_query = max(cpus // concurrency, 1)
    transfer_workers = min(128, max((cpus_per_query * 2) // 3, 1))
    if transfer_workers >= 8:
        transfer_workers = max((transfer_workers // 4) * 4, 4)
    background_worker_reserve = max(8, cpus // 6)
    desired_parallel_capacity = transfer_workers * concurrency
    max_parallel_workers = max(
        1, min(desired_parallel_capacity, max(cpus - background_worker_reserve, 1))
    )
    max_worker_processes = max(
        8, max_parallel_workers + background_worker_reserve
    )
    max_parallel_workers_per_gather = transfer_workers

    postgres = {
        "shared_buffers": pg_size(shared_buffers),
        "effective_cache_size": pg_size(effective_cache_size),
        "work_mem": pg_size(work_mem),
        "hash_mem_multiplier": "2",
        "maintenance_work_mem": pg_size(maintenance_work_mem),
        "max_worker_processes": str(max_worker_processes),
        "max_parallel_workers": str(max_parallel_workers),
        "max_parallel_workers_per_gather": str(max_parallel_workers_per_gather),
        "random_page_cost": "1.1" if storage == "ssd" else "4.0",
        "effective_io_concurrency": "256" if storage == "ssd" else "2",
        "huge_pages": "try",
    }
    bloompg = {
        "bloompg.materialization_memory": pg_size(materialization_memory),
        "bloompg.transfer_workers": str(transfer_workers),
        "bloompg.transfer_parallel_min_rows": "100000",
        "bloompg.sample_rate": "0.01",
        "bloompg.profile": "off",
        "bloompg.profile_log": "off",
    }
    return {
        "detected": {
            "memory_bytes": memory,
            "memory_gib": round(memory / GIB, 3),
            "cpus": cpus,
            "analytical_concurrency": concurrency,
            "active_memory_operators": active_operators,
            "storage": storage,
        },
        "postgresql": postgres,
        "bloompg": bloompg,
        "notes": [
            "work_mem is per sort/hash node and can be multiplied by workers.",
            "Bloom materialization memory is one total budget per query, including parallel DSM copies.",
            "Its recommendation divides a 50% private-memory pool by analytical query concurrency.",
            "Bloom transfer workers use PostgreSQL processes; the recommendation reserves one worker group per concurrent analytical query.",
            "bloompg.transfer_workers is a ceiling: scans scale down near 100k estimated input rows per worker and compatible same-wave relations share one Parallel Append group.",
            "Merge shared_preload_libraries with existing entries; do not overwrite them.",
            "Benchmark PostgreSQL and BloomPG with the same PostgreSQL settings.",
        ],
    }


def render_conf(result: dict) -> str:
    lines = [
        "# BloomPG analytical recommendations",
        "# Review concurrency assumptions before applying.",
        "",
        "# PostgreSQL settings",
    ]
    lines.extend(f"{key} = '{value}'" for key, value in result["postgresql"].items())
    lines.extend(("", "# BloomPG settings"))
    lines.extend(f"{key} = '{value}'" for key, value in result["bloompg"].items())
    lines.extend(("", "# Add bloompg to, rather than replace, existing preload libraries:",
                  "# shared_preload_libraries = '...,bloompg'"))
    return "\n".join(lines) + "\n"


def render_text(result: dict, source: str) -> str:
    detected = result["detected"]
    lines = [
        "BloomPG analytical tuning recommendations",
        f"  memory: {detected['memory_gib']:.3f} GiB ({source})",
        f"  CPUs: {detected['cpus']}",
        f"  analytical concurrency: {detected['analytical_concurrency']}",
        f"  assumed memory operators/query: {detected['active_memory_operators']}",
        f"  storage: {detected['storage']}",
        "",
        "PostgreSQL:",
    ]
    lines.extend(f"  {key} = {value}" for key, value in result["postgresql"].items())
    lines.extend(("", "BloomPG:"))
    lines.extend(f"  {key} = {value}" for key, value in result["bloompg"].items())
    lines.extend(("", "Notes:"))
    lines.extend(f"  - {note}" for note in result["notes"])
    lines.append("")
    lines.append("No settings were changed. Use --format conf --output PATH after review.")
    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()
    if args.memory_gib is not None and args.memory_gib <= 0:
        raise SystemExit("--memory-gib must be positive")
    if args.cpus is not None and args.cpus <= 0:
        raise SystemExit("--cpus must be positive")
    if args.analytical_concurrency <= 0:
        raise SystemExit("--analytical-concurrency must be positive")
    if args.active_memory_operators <= 0:
        raise SystemExit("--active-memory-operators must be positive")

    if args.memory_gib is None:
        memory, source = detected_memory_bytes()
    else:
        memory = int(args.memory_gib * GIB)
        source = "command line"
    cpus = args.cpus if args.cpus is not None else detected_cpus()
    result = recommend(
        memory,
        cpus,
        args.analytical_concurrency,
        args.active_memory_operators,
        args.storage,
    )

    if args.format == "json":
        rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    elif args.format == "conf":
        rendered = render_conf(result)
    else:
        rendered = render_text(result, source)

    if args.output is not None:
        args.output.write_text(rendered, encoding="utf-8")
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
