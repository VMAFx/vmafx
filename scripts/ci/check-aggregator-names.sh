#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# check-aggregator-names.sh — verify that required-aggregator.yml's required list
# and the set of # required-aggregator check names in .github/workflows/*.yml are identical.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"

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

agg_names = set(re.findall(r"'([^']+)'", req_match.group(1)))

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

print(f"OK: all {len(agg_names)} required checks in required-aggregator.yml match workflow definitions.")
PYEOF
