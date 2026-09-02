#!/usr/bin/env bash
#
# scaffold.sh — run the project modernization audit and write a dated report.
#
# Usage: bash .claude/skills/audit-modernization/scaffold.sh [--include-archives] [--max-findings=N] [--out=PATH]
#
# The audit is read-only. Output lands at /tmp/modernization-audit-YYYY-MM-DD.md
# unless --out is supplied.

set -euo pipefail

include_archives=0
max_findings=30
out_path=""

for arg in "$@"; do
  case "$arg" in
    --include-archives) include_archives=1 ;;
    --max-findings=*) max_findings="${arg#--max-findings=}" ;;
    --out=*) out_path="${arg#--out=}" ;;
    -h | --help)
      grep -E '^# ' "$0" | sed 's/^# //'
      exit 0
      ;;
    *)
      echo "error: unknown argument: $arg" >&2
      echo "usage: $0 [--include-archives] [--max-findings=N] [--out=PATH]" >&2
      exit 2
      ;;
  esac
done

if ! [[ "$max_findings" =~ ^[0-9]+$ ]]; then
  echo "error: --max-findings must be a positive integer; got '$max_findings'" >&2
  exit 2
fi

repo_root=$(git rev-parse --show-toplevel)
audit_script="$repo_root/scripts/dev/project_modernization_audit.py"

if [[ ! -f "$audit_script" ]]; then
  echo "error: audit script missing: $audit_script" >&2
  echo "hint: this skill wraps scripts/dev/project_modernization_audit.py — restore it before re-running." >&2
  exit 4
fi

date_stamp=$(date +%Y-%m-%d)
if [[ -z "$out_path" ]]; then
  out_path="/tmp/modernization-audit-${date_stamp}.md"
fi

mkdir -p "$(dirname "$out_path")"

# Build the audit invocation. The script's curated DEFAULT_SCAN_ROOTS and
# DEFAULT_STATE_FILES are exactly what we want; do NOT override them here.
cmd=(python3 "$audit_script" --repo-root "$repo_root"
  --out-md "$out_path"
  --max-findings "$max_findings")
if [[ $include_archives -eq 1 ]]; then
  cmd+=(--include-archives)
fi

echo "running modernization audit:"
printf '  %s\n' "${cmd[@]}"
echo

"${cmd[@]}"

if [[ ! -s "$out_path" ]]; then
  echo "error: audit produced empty output at $out_path" >&2
  exit 5
fi

echo
echo "audit report written: $out_path"
echo
echo "summary (first 5 lines):"
head -n 5 "$out_path" | sed 's/^/  /'
echo
echo "next steps:"
echo "  1. read the full report: less $out_path"
echo "  2. cross-reference with .workingdir2/BACKLOG.md for prioritization"
echo "  3. cross-reference with docs/state.md for bug-status overlap"
echo "  4. do NOT auto-dispatch agents on findings — the audit is a seed list, not a task queue"
