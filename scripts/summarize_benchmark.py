#!/usr/bin/env python3
"""Summarize one BloomPG benchmark JSONL file, including failures."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result", type=Path)
    parser.add_argument(
        "--show",
        type=int,
        default=5,
        help="Show this many lowest and highest individual speedups",
    )
    parser.add_argument("--json", action="store_true", help="Emit JSON only")
    return parser.parse_args()


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def load(path: Path) -> list[dict]:
    records: list[dict] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise SystemExit(f"{path}:{line_number}: {error}") from error
        records.append(record)
    return records


def summarize(records: list[dict]) -> dict:
    valid = [
        record
        for record in records
        if record.get("base", {}).get("status") == "ok"
        and record.get("bloom", {}).get("status") == "ok"
        and record.get("results_match") is True
    ]
    invalid = [record for record in records if record not in valid]
    result: dict = {
        "queries": len(records),
        "valid_pairs": len(valid),
        "result_matches": sum(record.get("results_match") is True for record in records),
        "invalid": [
            {
                "query": record.get("query"),
                "base_status": record.get("base", {}).get("status"),
                "bloom_status": record.get("bloom", {}).get("status"),
                "results_match": record.get("results_match"),
            }
            for record in invalid
        ],
    }
    if not valid:
        return result
    speedups = [record["speedup"] for record in valid]
    total_base = sum(record["base"]["median_ms"] for record in valid)
    total_bloom = sum(record["bloom"]["median_ms"] for record in valid)
    profiled = [
        record for record in valid
        if isinstance(record.get("bloom", {}).get("profile_ms"), dict)
    ]
    phase_names = ("planning", "transfer", "p1", "execution")
    phase_totals = {
        phase: sum(
            float(record["bloom"]["profile_ms"].get(phase) or 0.0)
            for record in profiled
        )
        for phase in phase_names
    }
    ranked = sorted(
        ({"query": record["query"], "speedup": record["speedup"]} for record in valid),
        key=lambda item: item["speedup"],
    )
    result.update(
        {
            "total_base_ms": total_base,
            "total_bloom_ms": total_bloom,
            "total_speedup": total_base / total_bloom,
            "geomean_speedup": math.exp(
                sum(math.log(value) for value in speedups) / len(speedups)
            ),
            "median_speedup": statistics.median(speedups),
            "p10_speedup": percentile(speedups, 0.10),
            "p90_speedup": percentile(speedups, 0.90),
            "faster": sum(value > 1.0 for value in speedups),
            "at_least_3x": sum(value >= 3.0 for value in speedups),
            "regressed_10pct": sum(value < (1.0 / 1.10) for value in speedups),
            "ranked": ranked,
            "bloom_profiled": len(profiled),
            "bloom_phase_totals_ms": phase_totals,
        }
    )
    return result


def main() -> int:
    args = parse_args()
    summary = summarize(load(args.result))
    if args.json:
        print(json.dumps(summary, sort_keys=True))
        return 0
    print(
        f"queries={summary['queries']} valid={summary['valid_pairs']} "
        f"matches={summary['result_matches']} invalid={len(summary['invalid'])}"
    )
    if summary.get("valid_pairs", 0):
        print(
            f"base={summary['total_base_ms']:.3f}ms "
            f"bloom={summary['total_bloom_ms']:.3f}ms "
            f"total={summary['total_speedup']:.4f}x "
            f"geomean={summary['geomean_speedup']:.4f}x "
            f"median={summary['median_speedup']:.4f}x "
            f"p10={summary['p10_speedup']:.4f}x "
            f"p90={summary['p90_speedup']:.4f}x"
        )
        print(
            f"faster={summary['faster']}/{summary['valid_pairs']} "
            f">=3x={summary['at_least_3x']}/{summary['valid_pairs']} "
            f">10% regressions={summary['regressed_10pct']}/{summary['valid_pairs']}"
        )
        if summary.get("bloom_profiled", 0):
            phases = summary["bloom_phase_totals_ms"]
            print(
                f"bloom_phases profiled={summary['bloom_profiled']} "
                f"planning={phases['planning']:.3f}ms "
                f"transfer={phases['transfer']:.3f}ms "
                f"p1={phases['p1']:.3f}ms "
                f"execution={phases['execution']:.3f}ms"
            )
        count = min(args.show, len(summary["ranked"]))
        print("lowest " + " ".join(
            f"{item['query']}={item['speedup']:.3f}x"
            for item in summary["ranked"][:count]
        ))
        print("highest " + " ".join(
            f"{item['query']}={item['speedup']:.3f}x"
            for item in summary["ranked"][-count:]
        ))
    if summary["invalid"]:
        print("invalid " + json.dumps(summary["invalid"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
