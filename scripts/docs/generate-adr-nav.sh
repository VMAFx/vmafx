#!/usr/bin/env bash
# Generate the ADR section of mkdocs.yml `nav:` as decade-grouped (per-hundred)
# collapsible blocks plus a by-tag index sub-tree.
#
# Inputs:
#   $REPO_ROOT/docs/adr/*.md             ADR files (NNNN-kebab-case-title.md)
#   $REPO_ROOT/docs/adr/by-tag/*.md      Pre-rendered by-tag indexes (see
#                                        generate-adr-by-tag.sh)
#
# Outputs (stdout): a YAML fragment ready to splice under `- ADRs:` in
# mkdocs.yml. Indented six spaces so it nests under the top-level nav entry.
#
# Flags:
#   --check   Compare against the in-tree mkdocs.yml ADR block; exit 1 on drift.
#   --write   Splice the generated block into mkdocs.yml in place.
#
# The block boundary in mkdocs.yml is marked with sentinel comments so
# round-trip edits are deterministic:
#
#   # >>> ADR-NAV-GENERATED — do not hand-edit; regenerate with
#   #     scripts/docs/generate-adr-nav.sh --write
#   ...generated block...
#   # <<< ADR-NAV-GENERATED
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
ADR_DIR="$REPO_ROOT/docs/adr"
BY_TAG_DIR="$ADR_DIR/by-tag"
MKDOCS_YML="$REPO_ROOT/mkdocs.yml"

render() {
  python3 - "$ADR_DIR" "$BY_TAG_DIR" <<'PY'
import json
import os
import re
import sys

adr_dir = sys.argv[1]
by_tag_dir = sys.argv[2]

# Collect ADRs by per-hundred bucket.
adrs = []
fname_re = re.compile(r"^(\d{4})-(.+)\.md$")
for name in sorted(os.listdir(adr_dir)):
    m = fname_re.match(name)
    if not m:
        continue
    if name == "0000-template.md":
        # Surfaced separately under Overview group
        continue
    adrs.append((int(m.group(1)), m.group(2), name))

buckets = {}
for num, slug, fname in adrs:
    b = num // 100
    buckets.setdefault(b, []).append((num, slug, fname))

# Display labels for the per-hundred buckets. Editorial — kept terse and
# anchored to the dominant theme(s) of each bucket. If a bucket later drifts
# away from its label, edit this map (the script is the source of truth).
LABELS = {
    0: "Foundation, build, golden gate",
    1: "Doc-substance, fragments, ports",
    2: "GPU coverage, tiny-AI scaffolding",
    3: "vmaf-tune, corpus ingestion, CI gates",
    4: "GPU parity, ffmpeg patches, MCP runtime",
    5: "Multi-vendor GPU, container, audits",
    6: "Saliency, predictor v2/v3, signal mix",
    7: "VMAFx rebrand, Go/Rust/C++23, k8s",
    8: "Backfill / pending sweeps",
}

# Bucket order: ascending numeric. Each bucket's entries also ascending.
lines = []
indent6 = "      "
indent8 = "        "
indent10 = "          "
indent12 = "            "

lines.append(f"{indent6}- Overview: adr/README.md")
lines.append(f"{indent6}- Template: adr/0000-template.md")

for b in sorted(buckets):
    lo = b * 100
    hi = lo + 99
    label = LABELS.get(b, "Misc")
    header = f'"{lo:04d}-{hi:04d} — {label}"'
    lines.append(f"{indent6}- {header}:")
    for num, slug, fname in buckets[b]:
        lines.append(f"{indent8}- {json.dumps(f'ADR-{num:04d}: {slug}')}: adr/{fname}")

# By-tag section: enumerate files under by-tag/, sorted.
if os.path.isdir(by_tag_dir):
    tag_files = sorted(
        f for f in os.listdir(by_tag_dir) if f.endswith(".md") and f != "index.md"
    )
    has_index = os.path.exists(os.path.join(by_tag_dir, "index.md"))
    if tag_files or has_index:
        lines.append(f"{indent6}- \"By tag\":")
        if has_index:
            lines.append(f"{indent8}- Overview: adr/by-tag/index.md")
        for f in tag_files:
            tag = f[:-3]  # strip .md
            lines.append(f"{indent8}- {json.dumps(tag)}: adr/by-tag/{f}")

print("\n".join(lines))
PY
}

mode="render"
case "${1:-}" in
  --check) mode="check" ;;
  --write) mode="write" ;;
  --help | -h)
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  "") ;;
  *)
    printf 'unknown flag: %s\n' "$1" >&2
    exit 64
    ;;
esac

rendered="$(render)"

if [[ "$mode" == render ]]; then
  printf '%s\n' "$rendered"
  exit 0
fi

# Check both sentinel boundaries before reading or replacing any bytes.
# Stage next to mkdocs.yml so replace is atomic even when /tmp is elsewhere.
printf '%s\n' "$rendered" | python3 -c '
from pathlib import Path
import difflib
import sys
import tempfile

path = Path(sys.argv[1])
mode = sys.argv[2]
lines = path.read_text().splitlines(keepends=True)
begins = [i for i, line in enumerate(lines) if line.lstrip().startswith("# >>> ADR-NAV-GENERATED")]
ends = [i for i, line in enumerate(lines) if line.lstrip().startswith("# <<< ADR-NAV-GENERATED")]
if len(begins) != 1 or len(ends) != 1 or begins[0] >= ends[0]:
    raise SystemExit("mkdocs.yml requires exactly one ordered ADR-NAV-GENERATED sentinel pair")
begin, end = begins[0], ends[0]
rendered = sys.stdin.read()
existing = "".join(lines[begin + 1:end])
if mode == "check":
    if existing != rendered:
        sys.stderr.writelines(difflib.unified_diff(existing.splitlines(True), rendered.splitlines(True), fromfile="committed ADR nav", tofile="generated ADR nav"))
        raise SystemExit("mkdocs.yml ADR nav is stale; run make docs-fragments-write")
else:
    replacement = "".join(lines[:begin + 1]) + rendered + "".join(lines[end:])
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix=".adr-nav-", delete=False) as output:
            temporary = Path(output.name)
            output.write(replacement)
        temporary.chmod(path.stat().st_mode)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print("mkdocs.yml ADR-nav block rewritten.", file=sys.stderr)
' "$MKDOCS_YML" "$mode"
