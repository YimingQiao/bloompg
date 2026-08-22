#!/usr/bin/env python3
"""Run SQL query files against PostgreSQL with BloomPG on and off.

Each mode gets a configurable number of unreported warmups followed by N timed
executions in the same backend.  Results are compared as row multisets and one
resumable JSON object is written per query.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PSQL = ROOT.parent / ".pg18" / "bin" / "psql"
TIMING_RE = re.compile(r"^Time:\s+([0-9]+(?:\.[0-9]+)?)\s+ms(?:\s|$)")
PROFILE_MARKER = "__BLOOMPG_PROFILE_JSON__"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--queries-dir", type=Path, required=True)
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="Discover SQL files recursively and use their relative path as query ID",
    )
    parser.add_argument("--database", default="bloompg_job")
    parser.add_argument(
        "--profile-label",
        default="unspecified",
        help="Human-readable physical/workload profile recorded in every row",
    )
    parser.add_argument("--host", default="/tmp")
    parser.add_argument("--port", type=int, default=55432)
    parser.add_argument("--psql", type=Path, default=DEFAULT_PSQL)
    parser.add_argument(
        "--pattern",
        default=r".*",
        help="Regex matched against the stable query ID (relative path when recursive)",
    )
    parser.add_argument(
        "--timed-runs",
        type=int,
        default=1,
        help="Measured executions after warmup (default: 1)",
    )
    parser.add_argument(
        "--warmups",
        type=int,
        default=1,
        help=(
            "Unreported executions before measurement (default: 1); use 0 "
            "for an externally prewarmed complete-pass protocol"
        ),
    )
    parser.add_argument(
        "--capture-profile",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "Capture BloomPG's phase profile after the final measured run "
            "without adding another workload execution"
        ),
    )
    parser.add_argument(
        "--statement-timeout-ms",
        type=int,
        default=0,
        help="Server and client timeout per execution in milliseconds; 0 disables it",
    )
    parser.add_argument(
        "--parallel-workers",
        type=int,
        default=0,
        help="Global and per-Gather PostgreSQL parallel-worker ceiling",
    )
    parser.add_argument(
        "--base-nestloop",
        choices=("default", "on", "off"),
        default="default",
        help=(
            "Override enable_nestloop only for native PostgreSQL; useful for "
            "recording a conservative baseline when its default plan stalls"
        ),
    )
    parser.add_argument(
        "--transfer-workers",
        type=int,
        help="Bloom transfer workers; defaults to --parallel-workers",
    )
    parser.add_argument(
        "--transfer-parallel-min-rows",
        type=int,
        default=100_000,
    )
    parser.add_argument(
        "--index-transfer",
        action=argparse.BooleanOptionalAction,
        default=False,
    )
    parser.add_argument("--index-transfer-max-fraction", type=float, default=0.15)
    parser.add_argument("--index-transfer-batch-keys", type=int, default=65_536)
    parser.add_argument("--index-transfer-max-keys", type=int, default=100_000)
    parser.add_argument(
        "--index-guard-min-rows",
        type=int,
        default=100_000,
        help=(
            "Base rows above which a relation with a join-key index waits "
            "for exact incoming keys (default: 100000)"
        ),
    )
    parser.add_argument(
        "--transfer-progress-metric",
        choices=("rows", "ndv"),
        default="ndv",
        help=(
            "Use legacy row-cardinality/lineage propagation or exact "
            "equality-domain NDV convergence (default: ndv)"
        ),
    )
    parser.add_argument(
        "--work-mem",
        help="Optional per-session PostgreSQL work_mem (for example 512MB)",
    )
    parser.add_argument(
        "--hash-mem-multiplier",
        type=float,
        help="Optional per-session PostgreSQL hash_mem_multiplier",
    )
    parser.add_argument("--sample-seed", type=int, default=2)
    parser.add_argument(
        "--sample-mode",
        choices=("prepared", "instant"),
        default="prepared",
        help="Bloom base-relation sampling mode (default: prepared)",
    )
    parser.add_argument(
        "--sample-size",
        type=int,
        default=10_000,
        help="Target rows in each base-relation sample (default: 10000)",
    )
    parser.add_argument(
        "--sample-rate",
        type=float,
        default=0.01,
        help="Chunk fraction sampled from already-materialized column stores",
    )
    parser.add_argument("--limit", type=int)
    parser.add_argument(
        "--queries-per-group",
        type=int,
        help=(
            "Deterministically select at most this many queries from each "
            "relative parent directory (useful for stratified CEB runs)"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "benchmark_results" / "pg_job.jsonl",
    )
    parser.add_argument(
        "--reuse-base-from",
        type=Path,
        help=(
            "Reuse a base result only when query ID and SQL SHA-256 match an "
            "existing JSONL record; otherwise run both modes."
        ),
    )
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def pgoptions(args: argparse.Namespace, enabled: bool) -> str:
    transfer_workers = (
        args.parallel_workers
        if args.transfer_workers is None
        else args.transfer_workers
    )
    options = [
        "-c session_preload_libraries=bloompg",
        f"-c bloompg.enable={'on' if enabled else 'off'}",
        f"-c bloompg.sample_mode={args.sample_mode}",
        f"-c bloompg.sample_size={args.sample_size}",
        f"-c bloompg.sample_seed={args.sample_seed}",
        f"-c bloompg.sample_rate={args.sample_rate}",
        f"-c max_parallel_workers={args.parallel_workers}",
        f"-c max_parallel_workers_per_gather={args.parallel_workers}",
        f"-c bloompg.transfer_workers={transfer_workers}",
        f"-c bloompg.transfer_parallel_min_rows={args.transfer_parallel_min_rows}",
        f"-c bloompg.index_transfer={'on' if args.index_transfer else 'off'}",
        f"-c bloompg.index_transfer_max_fraction={args.index_transfer_max_fraction}",
        f"-c bloompg.index_transfer_batch_keys={args.index_transfer_batch_keys}",
        f"-c bloompg.index_transfer_max_keys={args.index_transfer_max_keys}",
        f"-c bloompg.index_guard_min_rows={args.index_guard_min_rows}",
        f"-c bloompg.transfer_progress_metric={args.transfer_progress_metric}",
        f"-c bloompg.profile={'on' if args.capture_profile else 'off'}",
        "-c bloompg.profile_log=off",
    ]
    if args.work_mem:
        options.append(f"-c work_mem={args.work_mem}")
    if args.hash_mem_multiplier is not None:
        options.append(f"-c hash_mem_multiplier={args.hash_mem_multiplier}")
    if not enabled and args.base_nestloop != "default":
        options.append(f"-c enable_nestloop={args.base_nestloop}")
    return " ".join(options)


def canonical_result(lines: list[str]) -> str:
    return "\n".join(sorted(lines))


def result_hash(result: str) -> str:
    return hashlib.sha256(result.encode("utf-8")).hexdigest()


def parse_psql_output(stdout: str) -> tuple[list[float], list[str]]:
    timings: list[float] = []
    results: list[str] = []
    current_rows: list[str] = []
    for line in stdout.splitlines():
        match = TIMING_RE.match(line)
        if match:
            timings.append(float(match.group(1)))
            results.append(canonical_result(current_rows))
            current_rows = []
        else:
            current_rows.append(line)
    return timings, results


def split_profile_output(stdout: str) -> tuple[str, dict | None, str | None]:
    """Detach the out-of-band profile emitted after query timing is disabled."""
    lines = stdout.splitlines()
    marker_indexes = [
        index for index, line in enumerate(lines) if line == PROFILE_MARKER
    ]
    if not marker_indexes:
        return stdout, None, "profile marker not found"
    marker_index = marker_indexes[-1]
    profile_text = "\n".join(lines[marker_index + 1 :]).strip()
    query_stdout = "\n".join(lines[:marker_index])
    if stdout.endswith("\n"):
        query_stdout += "\n"
    if not profile_text:
        return query_stdout, None, "profile payload is empty"
    try:
        profile = json.loads(profile_text)
    except json.JSONDecodeError as error:
        return query_stdout, None, f"invalid profile JSON: {error}"
    if not isinstance(profile, dict):
        return query_stdout, None, "profile payload is not an object"
    return query_stdout, profile, None


def profile_breakdown_ms(profile: dict) -> dict:
    """Return the stable phase subset used in benchmark comparisons."""
    planning = profile.get("planning")
    if not isinstance(planning, dict):
        planning = {}
    executor = profile.get("executor")
    if not isinstance(executor, dict):
        executor = {}
    return {
        "planning": profile.get("planning_ms", planning.get("total_ms")),
        "p0": planning.get("p0_ms"),
        "native_preview": planning.get("native_preview_ms"),
        "graph": planning.get("graph_ms"),
        "transfer": planning.get("transfer_ms"),
        "statistics": planning.get("statistics_ms"),
        "p1": planning.get("p1_ms"),
        "execution": profile.get("execution_ms"),
        "executor_run": executor.get("run_ms"),
        "total": profile.get("total_ms"),
    }


def profile_is_terminal(profile: dict) -> bool:
    """Accept both an executed Bloom plan and a deliberate native no-op."""
    return profile.get("status") in {"complete", "empty"}


def terminate_sql(query_sql: str) -> str:
    """Return one psql-ready statement, accepting corpora without semicolons."""
    statement = query_sql.rstrip()
    if not statement.endswith(";"):
        statement += "\n;"
    return statement + "\n"


def run_mode(args: argparse.Namespace, query_sql: str, enabled: bool) -> dict:
    repetitions = args.warmups + args.timed_runs
    profile_suffix = ""
    if enabled and args.capture_profile:
        profile_suffix = (
            "\\timing off\n"
            f"\\echo {PROFILE_MARKER}\n"
            "SELECT bloompg_last_profile()::text;\n"
        )
    script = (
        f"SET statement_timeout = {args.statement_timeout_ms};\n"
        "\\pset pager off\n"
        "\\timing on\n" + terminate_sql(query_sql) * repetitions + profile_suffix
    )
    mode = "bloom" if enabled else "base"
    env = os.environ.copy()
    env["PGOPTIONS"] = pgoptions(args, enabled)
    env["LC_ALL"] = "C"
    command = [
        str(args.psql),
        "-h",
        args.host,
        "-p",
        str(args.port),
        "-d",
        args.database,
        "-X",
        "-qAt",
        "-v",
        "ON_ERROR_STOP=1",
    ]
    process_timeout = (
        None
        if args.statement_timeout_ms == 0
        else repetitions * (args.statement_timeout_ms / 1000 + 10)
    )
    try:
        completed = subprocess.run(
            command,
            input=script,
            text=True,
            capture_output=True,
            env=env,
            timeout=process_timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        return {
            "mode": mode,
            "status": "client_timeout",
            "error": str(error),
        }

    query_stdout = completed.stdout
    profile = None
    profile_error = None
    if enabled and args.capture_profile and completed.returncode == 0:
        query_stdout, profile, profile_error = split_profile_output(completed.stdout)
    timings, results = parse_psql_output(query_stdout)
    record: dict = {
        "mode": mode,
        "status": "ok" if completed.returncode == 0 else "error",
        "returncode": completed.returncode,
    }
    if completed.stderr:
        record["stderr"] = completed.stderr[-4000:]
    if completed.returncode != 0:
        return record
    if profile_error is not None:
        record["status"] = "profile_parse_error"
        record["profile_error"] = profile_error
        return record
    if len(timings) != repetitions or len(results) != repetitions:
        record["status"] = "parse_error"
        record["timings_found"] = timings
        record["result_batches_found"] = len(results)
        return record

    warmups = timings[: args.warmups]
    timed = timings[args.warmups :]
    hashes = [result_hash(result) for result in results]
    record.update(
        {
            "warmup_ms": warmups[0] if len(warmups) == 1 else None,
            "warmups_ms": warmups,
            "runs_ms": timed,
            "median_ms": statistics.median(timed),
            "result_hash": hashes[0],
            "result_stable": len(set(hashes)) == 1,
            "result_rows": 0 if results[0] == "" else results[0].count("\n") + 1,
        }
    )
    if profile is not None:
        record["profile"] = profile
        record["profile_ms"] = profile_breakdown_ms(profile)
        record["profile_noop"] = profile.get("status") == "empty"
        if not profile_is_terminal(profile):
            record["status"] = "profile_incomplete"
    return record


def read_settings(args: argparse.Namespace) -> dict:
    sql = """
SELECT json_build_object(
    'server_version', current_setting('server_version'),
    'shared_buffers', current_setting('shared_buffers'),
    'effective_cache_size', current_setting('effective_cache_size'),
    'work_mem', current_setting('work_mem'),
    'hash_mem_multiplier', current_setting('hash_mem_multiplier'),
    'base_enable_nestloop', current_setting('enable_nestloop'),
    'max_parallel_workers_per_gather',
        current_setting('max_parallel_workers_per_gather'),
    'max_parallel_workers', current_setting('max_parallel_workers'),
    'max_worker_processes', current_setting('max_worker_processes'),
    'random_page_cost', current_setting('random_page_cost'),
    'effective_io_concurrency', current_setting('effective_io_concurrency'),
    'bloompg_materialization_memory',
        current_setting('bloompg.materialization_memory'),
    'bloompg_sample_mode', current_setting('bloompg.sample_mode')
    ,'bloompg_sample_size', current_setting('bloompg.sample_size')
    ,'bloompg_sample_seed', current_setting('bloompg.sample_seed')
    ,'bloompg_sample_rate', current_setting('bloompg.sample_rate')
    ,'bloompg_sample_memory_cache', current_setting('bloompg.sample_memory_cache')
    ,'bloompg_transfer_workers', current_setting('bloompg.transfer_workers')
    ,'bloompg_transfer_parallel_min_rows',
        current_setting('bloompg.transfer_parallel_min_rows')
    ,'bloompg_index_transfer', current_setting('bloompg.index_transfer')
    ,'bloompg_index_transfer_max_fraction',
        current_setting('bloompg.index_transfer_max_fraction')
    ,'bloompg_index_transfer_batch_keys',
        current_setting('bloompg.index_transfer_batch_keys')
    ,'bloompg_index_transfer_max_keys',
        current_setting('bloompg.index_transfer_max_keys')
    ,'bloompg_index_guard_min_rows',
        current_setting('bloompg.index_guard_min_rows')
    ,'bloompg_transfer_progress_metric',
        current_setting('bloompg.transfer_progress_metric')
    ,'database', current_database()
    ,'database_bytes', pg_database_size(current_database())
    ,'user_index_count', (
        SELECT count(*) FROM pg_indexes
        WHERE schemaname NOT IN ('pg_catalog', 'information_schema')
    )
    ,'user_index_bytes', COALESCE((
        SELECT sum(pg_relation_size((quote_ident(schemaname) || '.' ||
            quote_ident(indexname))::regclass)) FROM pg_indexes
        WHERE schemaname NOT IN ('pg_catalog', 'information_schema')
    ), 0)
    ,'user_indexes', COALESCE((
        SELECT json_agg(indexdef ORDER BY schemaname, indexname)
        FROM pg_indexes
        WHERE schemaname NOT IN ('pg_catalog', 'information_schema')
    ), '[]'::json)
)::text;
"""
    env = os.environ.copy()
    env["PGOPTIONS"] = pgoptions(args, False)
    env["LC_ALL"] = "C"
    completed = subprocess.run(
        [
            str(args.psql),
            "-h",
            args.host,
            "-p",
            str(args.port),
            "-d",
            args.database,
            "-X",
            "-qAt",
            "-v",
            "ON_ERROR_STOP=1",
            "-c",
            sql,
        ],
        text=True,
        capture_output=True,
        env=env,
        check=False,
    )
    if completed.returncode != 0:
        return {"status": "error", "stderr": completed.stderr[-2000:]}
    try:
        return json.loads(completed.stdout.strip())
    except json.JSONDecodeError:
        return {"status": "parse_error", "stdout": completed.stdout[-2000:]}


def load_completed(path: Path) -> set[str]:
    completed: set[str] = set()
    if not path.is_file():
        return completed
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            completed.add(json.loads(line)["query"])
        except (json.JSONDecodeError, KeyError):
            continue
    return completed


def read_result_records(path: Path) -> list[dict]:
    records: list[dict] = []
    if not path.is_file():
        return records
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise SystemExit(
                f"invalid benchmark record at {path}:{line_number}: {error}"
            ) from error
        if not isinstance(record, dict):
            raise SystemExit(
                f"invalid benchmark record at {path}:{line_number}: expected object"
            )
        records.append(record)
    return records


def load_records(path: Path) -> dict[str, dict]:
    records: dict[str, dict] = {}
    if not path.is_file():
        raise SystemExit(f"reused base results not found: {path}")
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        try:
            record = json.loads(line)
            query = record["query"]
            record["base"]
        except (json.JSONDecodeError, KeyError, TypeError) as error:
            raise SystemExit(
                f"invalid reused base record at {path}:{line_number}: {error}"
            ) from error
        records[query] = record
    return records


def reusable_base(record: dict | None, query_sha256: str) -> dict | None:
    if record is None or record.get("query_sha256") != query_sha256:
        return None
    return record.get("base")


def query_id(root: Path, path: Path) -> str:
    """Return a stable, collision-free ID while preserving flat-workload IDs."""
    relative = path.relative_to(root).with_suffix("")
    return "__".join(relative.parts)


def evenly_spaced(paths: list[Path], maximum: int) -> list[Path]:
    """Choose deterministic coverage across a sorted template population."""
    if len(paths) <= maximum:
        return paths
    if maximum == 1:
        return [paths[len(paths) // 2]]
    indexes = {
        round(position * (len(paths) - 1) / (maximum - 1))
        for position in range(maximum)
    }
    return [paths[index] for index in sorted(indexes)]


def discover_queries(args: argparse.Namespace, matcher: re.Pattern[str]) -> list[Path]:
    candidates = sorted(
        args.queries_dir.rglob("*.sql")
        if args.recursive
        else args.queries_dir.glob("*.sql")
    )
    candidates = [
        path
        for path in candidates
        if matcher.fullmatch(query_id(args.queries_dir, path))
    ]
    if args.queries_per_group is not None:
        grouped: dict[Path, list[Path]] = {}
        for path in candidates:
            grouped.setdefault(path.relative_to(args.queries_dir).parent, []).append(
                path
            )
        candidates = [
            path
            for group in sorted(grouped)
            for path in evenly_spaced(grouped[group], args.queries_per_group)
        ]
    if args.limit is not None:
        candidates = candidates[: args.limit]
    return candidates


def summarize(records: list[dict]) -> None:
    valid = [
        record
        for record in records
        if record["base"].get("status") == "ok"
        and record["bloom"].get("status") == "ok"
        and record.get("results_match")
    ]
    if not valid:
        print("no valid paired results", file=sys.stderr)
        return
    speedups = [record["speedup"] for record in valid]
    total_base = sum(record["base"]["median_ms"] for record in valid)
    total_bloom = sum(record["bloom"]["median_ms"] for record in valid)
    geomean = math.exp(sum(math.log(value) for value in speedups) / len(speedups))
    print(
        f"paired={len(valid)} total_base_ms={total_base:.3f} "
        f"total_bloom_ms={total_bloom:.3f} total_speedup={total_base / total_bloom:.4f}x "
        f"geomean={geomean:.4f}x faster={sum(value > 1 for value in speedups)}/{len(valid)}"
    )


def main() -> int:
    args = parse_args()
    if args.timed_runs < 1:
        raise SystemExit("--timed-runs must be positive")
    if args.warmups < 0:
        raise SystemExit("--warmups must be non-negative")
    if args.statement_timeout_ms < 0:
        raise SystemExit("--statement-timeout-ms must be non-negative")
    if args.parallel_workers < 0:
        raise SystemExit("--parallel-workers must be non-negative")
    if args.transfer_workers is not None and args.transfer_workers < 0:
        raise SystemExit("--transfer-workers must be non-negative")
    if args.transfer_parallel_min_rows < 0:
        raise SystemExit("--transfer-parallel-min-rows must be non-negative")
    if not 0.0 <= args.index_transfer_max_fraction <= 1.0:
        raise SystemExit("--index-transfer-max-fraction must be between 0 and 1")
    if args.index_transfer_batch_keys < 1:
        raise SystemExit("--index-transfer-batch-keys must be positive")
    if args.index_transfer_max_keys < 1:
        raise SystemExit("--index-transfer-max-keys must be positive")
    if args.index_guard_min_rows < 0:
        raise SystemExit("--index-guard-min-rows must be non-negative")
    if args.queries_per_group is not None and args.queries_per_group < 1:
        raise SystemExit("--queries-per-group must be positive")
    if not 0.0 <= args.sample_rate <= 1.0:
        raise SystemExit("--sample-rate must be between 0 and 1")
    if args.sample_size < 1:
        raise SystemExit("--sample-size must be positive")
    if not args.psql.is_file():
        raise SystemExit(f"psql not found: {args.psql}")
    matcher = re.compile(args.pattern)
    queries = discover_queries(args, matcher)
    if not queries:
        raise SystemExit("no matching query files")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    completed = load_completed(args.output) if args.resume else set()
    reused_bases = load_records(args.reuse_base_from) if args.reuse_base_from else {}
    settings = read_settings(args)
    print(f"settings {json.dumps(settings, sort_keys=True)}", flush=True)
    if settings.get("status") in {"error", "parse_error"}:
        raise SystemExit(f"could not read effective benchmark settings: {settings}")
    expected_transfer_workers = (
        args.parallel_workers
        if args.transfer_workers is None
        else args.transfer_workers
    )
    effective_checks = {
        "max_parallel_workers": args.parallel_workers,
        "max_parallel_workers_per_gather": args.parallel_workers,
        "bloompg_sample_mode": args.sample_mode,
        "bloompg_sample_size": args.sample_size,
        "bloompg_sample_seed": args.sample_seed,
        "bloompg_sample_rate": args.sample_rate,
        "bloompg_transfer_workers": expected_transfer_workers,
        "bloompg_transfer_parallel_min_rows": args.transfer_parallel_min_rows,
        "bloompg_index_transfer": "on" if args.index_transfer else "off",
        "bloompg_index_transfer_max_fraction": args.index_transfer_max_fraction,
        "bloompg_index_transfer_batch_keys": args.index_transfer_batch_keys,
        "bloompg_index_transfer_max_keys": args.index_transfer_max_keys,
        "bloompg_index_guard_min_rows": args.index_guard_min_rows,
        "bloompg_transfer_progress_metric": args.transfer_progress_metric,
    }
    mismatches = {
        name: (expected, settings.get(name))
        for name, expected in effective_checks.items()
        if str(expected) != str(settings.get(name))
    }
    if mismatches:
        raise SystemExit(
            f"effective benchmark settings do not match request: {mismatches}"
        )
    if int(settings["max_worker_processes"]) < int(settings["max_parallel_workers"]):
        raise SystemExit(
            "max_worker_processes must be at least max_parallel_workers: "
            f"{settings['max_worker_processes']} < {settings['max_parallel_workers']}"
        )
    mode = "a" if args.resume else "w"
    with args.output.open(mode, encoding="utf-8") as output:
        for index, query_path in enumerate(queries):
            query_name = query_id(args.queries_dir, query_path)
            if query_name in completed:
                print(f"skip {query_name} (already recorded)", flush=True)
                continue
            query_sql = query_path.read_text(encoding="utf-8")
            query_sha256 = hashlib.sha256(query_sql.encode("utf-8")).hexdigest()
            modes: dict[str, dict] = {}
            reuse_record = reused_bases.get(query_name)
            reused_base = reusable_base(reuse_record, query_sha256)
            if reused_base is not None:
                modes["base"] = reused_base
                order = [True]
                print(
                    f"reuse {query_name} base from {args.reuse_base_from}",
                    flush=True,
                )
            else:
                if reuse_record is not None:
                    print(
                        f"ignore stale base for {query_name}: SQL SHA-256 differs",
                        flush=True,
                    )
                order = [False, True] if index % 2 == 0 else [True, False]
            for enabled in order:
                label = "bloom" if enabled else "base"
                print(f"run {query_name} {label}", flush=True)
                modes[label] = run_mode(args, query_sql, enabled)
            base = modes["base"]
            bloom = modes["bloom"]
            record = {
                "query": query_name,
                "query_sha256": query_sha256,
                "profile_label": args.profile_label,
                "settings": settings,
                "base": base,
                "bloom": bloom,
            }
            if reused_base is not None:
                record["base_reused_from"] = str(args.reuse_base_from)
            if base.get("status") == "ok" and bloom.get("status") == "ok":
                record["results_match"] = (
                    base["result_stable"]
                    and bloom["result_stable"]
                    and base["result_hash"] == bloom["result_hash"]
                )
                record["speedup"] = base["median_ms"] / bloom["median_ms"]
                print(
                    f"done {query_name} base={base['median_ms']:.3f}ms "
                    f"bloom={bloom['median_ms']:.3f}ms speedup={record['speedup']:.3f}x "
                    f"match={record['results_match']}",
                    flush=True,
                )
            else:
                record["results_match"] = False
                print(
                    f"failed {query_name} base={base.get('status')} bloom={bloom.get('status')}",
                    flush=True,
                )
            output.write(json.dumps(record, sort_keys=True) + "\n")
            output.flush()
    summarize(read_result_records(args.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
