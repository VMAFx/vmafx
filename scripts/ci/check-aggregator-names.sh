#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# check-aggregator-names.sh — verify that required-aggregator.yml's required list
# and the set of # required-aggregator check names in .github/workflows/*.yml are identical,
# and that every required name is reported by exactly one job. The aggregator keeps one check
# run per name (the newest), so two jobs sharing a required name can mask each other's failure
# (T-CI-MSVC-CUDA-SHARED-CHECK-NAME-2026-09-18).
#
# Usage: check-aggregator-names.sh [repo-root]   (the argument exists for the fixture tests)

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${1:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}"

python3 - "$REPO_ROOT" <<'PYEOF'
import re
import sys
from pathlib import Path

repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
agg_file = repo_root / ".github" / "workflows" / "required-aggregator.yml"
agg_text = agg_file.read_text(encoding="utf-8")

req_match = re.search(r"const required = \[(.*?)\];", agg_text, re.DOTALL)
if not req_match:
    sys.exit("error: could not find 'const required = [...]' in required-aggregator.yml")

# Strip `//` line comments before extracting the literals. An ordinary English
# apostrophe in a comment ("one pull request's rollup") otherwise opens a
# spurious quoted run, and the mismatch it produces names a multi-line blob
# rather than the real problem, which costs the reader more time than the
# comment saved. Only whole-line comments are stripped: a trailing `//` after a
# literal cannot contain one without the literal having closed first.
req_body = re.sub(r"(?m)^\s*//.*$", "", req_match.group(1))
agg_names = set(re.findall(r"'([^']+)'", req_body))

wf_names = set()
for p in sorted((repo_root / ".github" / "workflows").glob("*.yml")):
    if p.name == "required-aggregator.yml":
        continue
    lines = p.read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"^\s*#\s*required-aggregator:\s*(.+)$", line)
        if m:
            wf_names.add(m.group(1).strip())
        elif re.match(r"^\s*#\s*required-aggregator\s*$", line) and i + 1 < len(lines):
            for j in range(i + 1, min(i + 12, len(lines))):
                nm = re.search(r"^\s*(?:-\s*)?name:\s*[\"']?([^\"'\n]+)[\"']?", lines[j])
                if nm:
                    wf_names.add(nm.group(1).strip())
                    break

diff = agg_names ^ wf_names
if diff:
    msg = ["Aggregator / workflow check names mismatch:"]
    if agg_names - wf_names:
        msg.append("  Missing in workflows (in aggregator only):")
        for n in sorted(agg_names - wf_names):
            msg.append(f"    - '{n}'")
    if wf_names - agg_names:
        msg.append("  Missing in aggregator (in workflows only):")
        for n in sorted(wf_names - agg_names):
            msg.append(f"    - '{n}'")
    sys.exit("\n".join(msg))

def job_names(path: Path) -> list[str]:
    """Every job or matrix-row display name in a workflow.

    Step names are not check names, so everything nested under a `steps:` key
    is skipped, as is the workflow's own top-level `name:`.
    """
    names = []
    steps_indent = None
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.lstrip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(stripped)
        if steps_indent is not None:
            if indent > steps_indent or (indent == steps_indent and stripped.startswith("- ")):
                continue
            steps_indent = None
        if re.match(r"steps:\s*(#.*)?$", stripped):
            steps_indent = indent
            continue
        m = re.match(r"(?:-\s*)?name:\s*[\"']?([^\"'#\n]+?)[\"']?\s*(?:#.*)?$", stripped)
        if m and indent > 0:
            names.append(m.group(1).strip())
    return names


reporters: dict[str, list[str]] = {}
for p in sorted((repo_root / ".github" / "workflows").glob("*.yml")):
    if p.name == "required-aggregator.yml":
        continue
    for name in job_names(p):
        if name in agg_names:
            reporters.setdefault(name, []).append(p.name)
shared = {n: files for n, files in reporters.items() if len(files) > 1}
if shared:
    msg = ["Required check names reported by more than one job (the aggregator keeps only the newest run per name):"]
    for n in sorted(shared):
        msg.append(f"  - '{n}': {', '.join(shared[n])}")
    sys.exit("\n".join(msg))

print(f"OK: all {len(agg_names)} required checks in required-aggregator.yml match workflow definitions, each reported by one job.")
PYEOF
