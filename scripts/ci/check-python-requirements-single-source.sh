#!/usr/bin/env bash
# scripts/ci/check-python-requirements-single-source.sh
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# Enforce that python/pyproject.toml [project].dependencies is the single source
# of truth for python/requirements.txt.
#
# Mode:
#   scripts/ci/check-python-requirements-single-source.sh          # verify (exit 1 on drift)
#   scripts/ci/check-python-requirements-single-source.sh --write  # regenerate python/requirements.txt
#   make python-deps-sync                                          # make shortcut
set -euo pipefail
export LC_ALL=C

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

pyproject="python/pyproject.toml"
reqs="python/requirements.txt"

if [ ! -f "$pyproject" ]; then
  echo "error: $pyproject not found" >&2
  exit 2
fi

mode="check"
if [ "${1:-}" = "--write" ]; then
  mode="write"
fi

python3 - "$mode" "$pyproject" "$reqs" <<'PYEOF'
import sys
from pathlib import Path
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib

mode, pyproject_path, reqs_path = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])

with open(pyproject_path, "rb") as f:
    data = tomllib.load(f)

deps = data.get("project", {}).get("dependencies", [])
if not deps:
    print(f"error: no [project].dependencies found in {pyproject_path}", file=sys.stderr)
    sys.exit(2)

expected_lines = [
    "# Generated from python/pyproject.toml [project].dependencies by scripts/ci/check-python-requirements-single-source.sh.",
    "# DO NOT EDIT DIRECTLY. python/pyproject.toml is the single owner.",
    "# To regenerate: make python-deps-sync (or scripts/ci/check-python-requirements-single-source.sh --write)",
] + deps + [""]
expected_content = "\n".join(expected_lines)

if mode == "write":
    reqs_path.write_text(expected_content, encoding="utf-8")
    print(f"check-python-requirements-single-source: wrote {len(deps)} dependencies to {reqs_path}")
    sys.exit(0)

# Check mode
if not reqs_path.is_file():
    print(f"error: {reqs_path} missing. Run 'make python-deps-sync' or '{sys.argv[0]} --write'", file=sys.stderr)
    sys.exit(1)

# Read actual lines, ignoring leading comments or blank lines for strict dependency comparison
actual_raw = reqs_path.read_text(encoding="utf-8")
actual_deps = [
    line.strip() for line in actual_raw.splitlines()
    if line.strip() and not line.strip().startswith("#")
]

if actual_deps != deps:
    print("::error title=python dependency drift::python/requirements.txt has drifted from python/pyproject.toml", file=sys.stderr)
    print("--- python/pyproject.toml (authority) vs python/requirements.txt ---", file=sys.stderr)
    import difflib
    for d in difflib.unified_diff(deps, actual_deps, fromfile="pyproject.toml", tofile="requirements.txt"):
        print(d, file=sys.stderr)
    print(f"\nRun 'make python-deps-sync' or '{sys.argv[0]} --write' to re-sync.", file=sys.stderr)
    sys.exit(1)

print(f"check-python-requirements-single-source: OK ({len(deps)} dependencies in sync)")
PYEOF
