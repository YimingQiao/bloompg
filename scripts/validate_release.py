#!/usr/bin/env python3
"""Validate BloomPG release metadata and extension upgrade paths."""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict, deque
from pathlib import Path

VERSION_PATTERN = r"[0-9]+\.[0-9]+\.[0-9]+"
INSTALL_SQL_PATTERN = re.compile(rf"^bloompg--({VERSION_PATTERN})\.sql$")
UPGRADE_SQL_PATTERN = re.compile(
    rf"^bloompg--({VERSION_PATTERN})--({VERSION_PATTERN})\.sql$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--tag", help="Require this tag to match v<release version>")
    parser.add_argument("--print-version", action="store_true")
    return parser.parse_args()


def require_match(
    errors: list[str], text: str, pattern: str, description: str
) -> str | None:
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        errors.append(f"missing {description}")
        return None
    return match.group(1)


def upgrade_reaches(start: str, destination: str, edges: dict[str, set[str]]) -> bool:
    pending = deque([start])
    visited = {start}
    while pending:
        version = pending.popleft()
        if version == destination:
            return True
        for successor in edges.get(version, set()):
            if successor not in visited:
                visited.add(successor)
                pending.append(successor)
    return False


def validate(root: Path, tag: str | None = None) -> tuple[str, list[str]]:
    errors: list[str] = []
    meta = json.loads((root / "META.json").read_text(encoding="utf-8"))
    version = str(meta.get("version", ""))
    if re.fullmatch(VERSION_PATTERN, version) is None:
        errors.append(f"META.json has invalid semantic version {version!r}")

    control = (root / "bloompg.control").read_text(encoding="utf-8")
    header = (root / "src/include/bloompg/bloompg.h").read_text(encoding="utf-8")
    readme = (root / "README.md").read_text(encoding="utf-8")
    changelog = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    makefile = (root / "Makefile").read_text(encoding="utf-8")

    declared = {
        "bloompg.control": require_match(
            errors,
            control,
            rf"^default_version\s*=\s*'({VERSION_PATTERN})'\s*$",
            "default_version in bloompg.control",
        ),
        "C header": require_match(
            errors,
            header,
            rf'^#define BLOOMPG_VERSION "({VERSION_PATTERN})"$',
            "BLOOMPG_VERSION in bloompg.h",
        ),
        "README badge": require_match(
            errors,
            readme,
            rf"Version ({VERSION_PATTERN})",
            "version badge in README.md",
        ),
        "CHANGELOG": require_match(
            errors,
            changelog,
            rf"^## ({VERSION_PATTERN}) - [0-9]{{4}}-[0-9]{{2}}-[0-9]{{2}}$",
            "top release heading in CHANGELOG.md",
        ),
    }
    for source, declared_version in declared.items():
        if declared_version is not None and declared_version != version:
            errors.append(f"{source} declares {declared_version}, expected {version}")

    provided = meta.get("provides", {}).get("bloompg", {})
    if provided.get("version") != version:
        errors.append("META.json provides.bloompg.version does not match version")
    expected_install = f"sql/bloompg--{version}.sql"
    if provided.get("file") != expected_install:
        errors.append(f"META.json must provide {expected_install}")
    if not (root / expected_install).is_file():
        errors.append(f"missing installation script {expected_install}")

    sql_directory = root / "sql"
    install_versions: set[str] = set()
    upgrade_edges: dict[str, set[str]] = defaultdict(set)
    sql_paths = sorted(sql_directory.glob("bloompg--*.sql"))
    for path in sql_paths:
        install_match = INSTALL_SQL_PATTERN.fullmatch(path.name)
        if install_match:
            install_versions.add(install_match.group(1))
            continue
        upgrade_match = UPGRADE_SQL_PATTERN.fullmatch(path.name)
        if upgrade_match:
            upgrade_edges[upgrade_match.group(1)].add(upgrade_match.group(2))
            continue
        errors.append(f"unrecognized extension SQL filename {path.name}")

    if version not in install_versions:
        errors.append(f"no installation SQL for release {version}")
    for installed in sorted(install_versions):
        if not upgrade_reaches(installed, version, upgrade_edges):
            errors.append(f"no upgrade path from {installed} to {version}")
    for path in sql_paths:
        relative = path.relative_to(root).as_posix()
        if relative not in makefile:
            errors.append(f"Makefile DATA omits {relative}")

    if tag is not None and tag != f"v{version}":
        errors.append(f"tag {tag!r} does not match release v{version}")
    return version, errors


def main() -> int:
    args = parse_args()
    version, errors = validate(args.root.resolve(), args.tag)
    if args.print_version:
        print(version)
    if errors:
        for error in errors:
            print(f"release validation: {error}")
        return 1
    if not args.print_version:
        print(f"BloomPG {version} release metadata is consistent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
