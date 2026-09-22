#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# hiss-audit.sh — run praetorctl's HISS audit with a touched-file set that
# names what the committer actually wrote.
#
# Why this wrapper exists (ADR-1298). praetorctl derives its touched-file set
# from `git diff --name-only HEAD`. During a merge HEAD is still the pre-merge
# tip, so that diff names every file the *other* side changed, and the
# touched-file-must-be-clean rule is then applied to code the committer never
# wrote. Merging master into any branch therefore failed on master's own
# baselined debt: measured on the #1518 restack, 29 findings across
# core/src/feature/x86/adm_avx2.c (11), adm_avx512.c (11) and
# core/src/feature/sycl/integer_adm_sycl.cpp (7), alongside `0 new
# unbaselined` — the ratchet satisfied, only the boy-scout rule firing.
#
# CI does not have this problem and does not apply the rule at all: it runs the
# bare form on a clean checkout, where the diff against HEAD is empty.
# .github/workflows/standards-gate.yml records that as deliberate ("Deliberately
# the baseline-only form... Revisit `-base` once the baseline is low enough that
# touching a file is realistic"). So the local hook was enforcing something the
# declared policy does not, purely as an artefact of running against a dirty
# tree.
#
# For an ordinary commit the two agree, so nothing is passed and praetorctl asks
# git as before. Only the merge case is corrected, by naming the files whose
# merged content differs from what the merged-in side supplied — i.e. the
# conflict resolutions. A file taken verbatim from the other parent is that
# parent's work and is already covered by the ratchet.

set -euo pipefail

run_audit() {
  if command -v praetorctl >/dev/null 2>&1; then
    praetorctl audit "$@"
  elif [ -d ./cmd/standardsctl ]; then
    go run ./cmd/standardsctl audit "$@"
  else
    echo "HISS-21 governance hook cannot run because praetorctl is not installed" >&2
    exit 1
  fi
}

merge_head="$(git rev-parse --git-path MERGE_HEAD)"
if [ ! -f "$merge_head" ]; then
  run_audit
  exit $?
fi

# Files whose staged content differs from the merged-in side: the resolutions.
resolved=""
while IFS= read -r path; do
  [ -n "$path" ] || continue
  if ! git diff --quiet MERGE_HEAD -- "$path" 2>/dev/null; then
    resolved="${resolved:+$resolved,}$path"
  fi
done < <(git diff --cached --name-only)

if [ -z "$resolved" ]; then
  echo "hiss-audit: merge contributed no resolutions of its own; auditing the baseline only." >&2
  # A path that matches nothing yields an empty touched set without falling
  # back to the misleading `git diff HEAD`.
  run_audit -touched "$(git rev-parse --git-path MERGE_HEAD)"
  exit $?
fi

echo "hiss-audit: merge in progress; touched-file rule scoped to the $(printf '%s' "$resolved" | tr ',' '\n' | grep -c .) resolved path(s)." >&2
run_audit -touched "$resolved"
