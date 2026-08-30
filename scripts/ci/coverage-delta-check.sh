#!/usr/bin/env bash
# Per-PR coverage-delta gate (ADR-0922).
#
# Compares two gcovr --json-summary reports — `base` (the PR's merge-base
# tip on the target branch) and `head` (the PR's head commit) — and fails
# if either:
#   * overall line coverage dropped by more than --max-overall-drop pp
#     (default 0.5pp), OR
#   * any file present in BOTH reports AND touched by the PR's diff
#     dropped by more than --max-file-drop pp (default 0.5pp).
#
# Files added by the PR are NOT compared (no base reading to compare to);
# the absolute per-file floors in coverage-check.sh already gate them.
# Files removed by the PR are skipped — they have no head row to score.
#
# This gate complements coverage-check.sh:
#   * coverage-check.sh enforces ABSOLUTE floors (overall, critical).
#   * coverage-delta-check.sh enforces a RATCHET — no PR is allowed to
#     erode coverage on files it touches without an explicit waiver.
#
# Usage:
#   scripts/ci/coverage-delta-check.sh \
#       --base-json <base-coverage.json> \
#       --head-json <head-coverage.json> \
#       --changed-files <newline-separated-paths> \
#       [--max-overall-drop 0.5] \
#       [--max-file-drop 0.5]
#
# Exit codes:
#   0 — gate passed (no drop exceeds the tolerance, or PR touches no .c/.h)
#   1 — overall coverage dropped past --max-overall-drop
#   2 — at least one touched file dropped past --max-file-drop
#   3 — argument / IO error
#
# See ADR-0922 for the rationale and the 30-day grace window for PRs that
# were already open when the ratchet landed.

set -euo pipefail

MAX_OVERALL_DROP="0.5"
MAX_FILE_DROP="0.5"
BASE_JSON=""
HEAD_JSON=""
CHANGED_FILES=""

usage() {
  sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'
  exit 3
}

while [ $# -gt 0 ]; do
  case "$1" in
    --base-json)
      BASE_JSON="${2:-}"
      shift 2
      ;;
    --head-json)
      HEAD_JSON="${2:-}"
      shift 2
      ;;
    --changed-files)
      CHANGED_FILES="${2:-}"
      shift 2
      ;;
    --max-overall-drop)
      MAX_OVERALL_DROP="${2:-}"
      shift 2
      ;;
    --max-file-drop)
      MAX_FILE_DROP="${2:-}"
      shift 2
      ;;
    -h | --help) usage ;;
    *)
      echo "unknown arg: $1" >&2
      usage
      ;;
  esac
done

if [ -z "$BASE_JSON" ] || [ -z "$HEAD_JSON" ]; then
  echo "error: both --base-json and --head-json are required" >&2
  usage
fi

if [ ! -f "$BASE_JSON" ]; then
  echo "error: base coverage JSON not found: $BASE_JSON" >&2
  exit 3
fi
if [ ! -f "$HEAD_JSON" ]; then
  echo "error: head coverage JSON not found: $HEAD_JSON" >&2
  exit 3
fi

if ! command -v python3 >/dev/null; then
  echo "error: python3 not installed — cannot parse gcovr JSON" >&2
  exit 3
fi

# Build a newline-separated list of changed files. Empty list = no PR diff
# in the coverage scope, gate passes trivially.
CHANGED_FILES_INPUT=""
if [ -n "$CHANGED_FILES" ]; then
  if [ -f "$CHANGED_FILES" ]; then
    CHANGED_FILES_INPUT="$(cat "$CHANGED_FILES")"
  else
    # Treat as inline newline-or-whitespace separated list.
    CHANGED_FILES_INPUT="$CHANGED_FILES"
  fi
fi

# Hand off to python for the actual delta math. Keeping the heavy
# lifting in one process avoids fragile multi-pass awk/jq.
export DELTA_CHANGED_FILES="$CHANGED_FILES_INPUT"
rc=0
python3 - "$BASE_JSON" "$HEAD_JSON" "$MAX_OVERALL_DROP" "$MAX_FILE_DROP" <<'PYEOF' || rc=$?
import json
import os
import sys

base_path = sys.argv[1]
head_path = sys.argv[2]
max_overall_drop = float(sys.argv[3])
max_file_drop = float(sys.argv[4])

changed_files_raw = os.environ.get("DELTA_CHANGED_FILES", "")
changed = set()
for line in changed_files_raw.splitlines():
    line = line.strip()
    if not line:
        continue
    # Normalise — strip leading ./ and any ../ that may appear if the
    # caller piped through "git diff --name-only" from a subdir.
    while line.startswith("./"):
        line = line[2:]
    while line.startswith("../"):
        line = line[3:]
    changed.add(line)

with open(base_path) as f:
    base = json.load(f)
with open(head_path) as f:
    head = json.load(f)

base_overall = float(base.get("line_percent", 0.0))
head_overall = float(head.get("line_percent", 0.0))
overall_delta = head_overall - base_overall

print("Coverage-delta gate (ADR-0922)")
print(f"  Base overall: {base_overall:.4f}%")
print(f"  Head overall: {head_overall:.4f}%")
print(f"  Overall delta: {overall_delta:+.4f}pp "
      f"(tolerance: -{max_overall_drop}pp)")

def index(entries):
    out = {}
    for e in entries or []:
        fn = e.get("filename", "")
        if not fn:
            continue
        # gcovr emits paths relative to its --root; strip a leading ../
        # so they align with "git diff --name-only" output.
        key = fn
        while key.startswith("./"):
            key = key[2:]
        while key.startswith("../"):
            key = key[3:]
        out[key] = e
    return out

base_files = index(base.get("files", []))
head_files = index(head.get("files", []))

overall_fail = overall_delta < -max_overall_drop - 1e-9

file_failures = []
file_drops_reported = 0
file_drops_within_tolerance = 0
file_skipped_no_base = 0
file_skipped_not_changed = 0

# Build the candidate set: any file that has a head row AND was touched
# by the PR. We compare suffixes too because git uses repo-root-relative
# paths while gcovr may emit paths relative to its --root (so "src/foo.c"
# in gcovr vs "core/src/foo.c" in git).
def matches_changed(path):
    if path in changed:
        return True
    # Suffix-match: gcovr may emit "src/foo.c" while git emits
    # "core/src/foo.c" or vice versa. Anchor on the trailing component
    # plus at least one directory to avoid coincidental matches.
    for c in changed:
        if c.endswith("/" + path) or path.endswith("/" + c):
            return True
    return False

for path, head_row in sorted(head_files.items()):
    if not matches_changed(path):
        file_skipped_not_changed += 1
        continue
    if path not in base_files:
        # New file (or moved/renamed). Absolute floors in coverage-check.sh
        # cover the new-file case; delta gate has nothing to compare.
        file_skipped_no_base += 1
        print(f"  skip (new file, no base): {path} "
              f"@ {head_row.get('line_percent', 0.0):.4f}%")
        continue
    base_pct = float(base_files[path].get("line_percent", 0.0))
    head_pct = float(head_row.get("line_percent", 0.0))
    delta = head_pct - base_pct
    if delta < -max_file_drop - 1e-9:
        file_failures.append((path, base_pct, head_pct, delta))
        file_drops_reported += 1
        print(f"  FAIL: {path}: {base_pct:.4f}% -> {head_pct:.4f}% "
              f"({delta:+.4f}pp, tolerance -{max_file_drop}pp)")
    elif delta < 0:
        file_drops_within_tolerance += 1
        print(f"  ok (within tolerance): {path}: "
              f"{base_pct:.4f}% -> {head_pct:.4f}% ({delta:+.4f}pp)")
    else:
        print(f"  ok (non-negative): {path}: "
              f"{base_pct:.4f}% -> {head_pct:.4f}% ({delta:+.4f}pp)")

print()
print(f"Touched-file summary: "
      f"{len(file_failures)} fail, "
      f"{file_drops_within_tolerance} small drop (within tolerance), "
      f"{file_skipped_no_base} new (no base row), "
      f"{file_skipped_not_changed} files-with-head-but-not-in-diff (ignored)")

if overall_fail and file_failures:
    print()
    print("FAIL: overall coverage dropped past the gate AND at least one "
          "touched file dropped past the per-file gate.", file=sys.stderr)
    print(
        "To resolve: either add tests that lift the touched files back "
        "above the base, or open a follow-up ADR superseding ADR-0922 to "
        "justify the new floor. The ratchet is one-way by design — see "
        "docs/adr/0922-coverage-ratchet-aggressive.md.",
        file=sys.stderr,
    )
    sys.exit(1)  # report the overall failure first; both visible above
if overall_fail:
    print()
    print(
        f"FAIL: overall line coverage dropped {-overall_delta:.4f}pp "
        f"(base {base_overall:.4f}% -> head {head_overall:.4f}%), exceeding "
        f"the {max_overall_drop}pp ratchet tolerance set by ADR-0922.",
        file=sys.stderr,
    )
    print(
        "To resolve: add tests that cover the new/changed lines this PR "
        "introduced, or open a follow-up ADR superseding ADR-0922 if the "
        "drop is structurally unavoidable. The delta gate is one-way by "
        "design.",
        file=sys.stderr,
    )
    sys.exit(1)
if file_failures:
    print()
    print(
        f"FAIL: {len(file_failures)} touched file(s) dropped past the "
        f"{max_file_drop}pp ratchet tolerance set by ADR-0922.",
        file=sys.stderr,
    )
    for path, base_pct, head_pct, delta in file_failures:
        print(
            f"  {path}: {base_pct:.4f}% -> {head_pct:.4f}% ({delta:+.4f}pp)",
            file=sys.stderr,
        )
    print(
        "To resolve: add tests that bring each listed file back to (or "
        "above) its base coverage, or open a follow-up ADR superseding "
        "ADR-0922 if the drop is structurally unavoidable.",
        file=sys.stderr,
    )
    sys.exit(2)

print("PASS: coverage-delta gate met (no touched-file or overall drop "
      f"exceeded {max_file_drop}pp / {max_overall_drop}pp respectively).")
PYEOF
rc=${rc:-0}
exit "$rc"
