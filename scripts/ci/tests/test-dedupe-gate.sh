#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
# Protect the required and local duplicate-implementation gate wiring.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf -- "$tmp_dir"' EXIT
fail=0

report_failure() {
  echo "dedupe-gate-contract: $*" >&2
  fail=$((fail + 1))
}

standards_step="$(awk '
  /^      - name: Reject duplicate implementation families$/ { capture = 1 }
  capture && /^      - / && $0 !~ /Reject duplicate implementation families$/ { exit }
  capture { print }
' "$repo_root/.github/workflows/standards-gate.yml")"

grep -Fq 'run: standardsctl dedupe scan .' <<<"$standards_step" ||
  report_failure "required Standards step does not invoke the clone scan"
if grep -Eq 'continue-on-error|\|\| true' <<<"$standards_step"; then
  report_failure "required Standards step masks clone-scan failure"
fi
grep -Fq '# required-aggregator: Standards & Invariant Verification Gate' \
  "$repo_root/.github/workflows/standards-gate.yml" ||
  report_failure "Standards job is not marked for the required aggregator"

grep -Fq $'dedupe-check:\n\t@standardsctl dedupe scan .' "$repo_root/Makefile" ||
  report_failure "Make dedupe-check target is missing"
grep -Fq $'\t@$(MAKE) --no-print-directory dedupe-check' "$repo_root/Makefile" ||
  report_failure "make verify-all does not invoke dedupe-check"

lefthook_count="$(grep -Fc 'sh .config/lefthook/engine.sh dedupe scan .' "$repo_root/lefthook.yml")"
if [[ "$lefthook_count" -ne 2 ]]; then
  report_failure "expected clone scan in pre-commit and pre-push, found $lefthook_count"
fi

# A test runs once in CI (ADR-1568): the required Tooling Tests job runs this
# file because the registry puts it in the tooling suite. The listing is read
# whole before it is searched: piped into `grep -q` under pipefail, the writer
# could die of SIGPIPE once grep had its match, and the check failed at random.
tooling_suite=$(python3 "$repo_root/scripts/ci/suite_registry.py" --root "$repo_root" list tooling)
grep -Fqx 'scripts/ci/tests/test-dedupe-gate.sh' <<<"$tooling_suite" ||
  report_failure "contract test is not in the tooling suite that Tooling Tests runs"
grep -Fq 'id: dedupe-gate-contract' "$repo_root/.pre-commit-config.yaml" ||
  report_failure "contract test is not wired into local hooks"

# HISS-21: a native Windows make resolves standardsctl itself and cannot run the
# POSIX stub below, so the replay would exercise the real engine instead.
if make --version 2>/dev/null | grep -Eqi 'windows32|mingw'; then
  echo "dedupe-gate-contract: skipped the make verify-all replay: $(command -v make) is a" \
    "native Windows make; the Linux Deliverables Checklist job in rule-enforcement.yml runs it" >&2
  [[ "$fail" -eq 0 ]]
  exit
fi

cat >"$tmp_dir/standardsctl" <<'SH'
#!/bin/sh
set -eu
printf '%s\n' "$*" >>"$DEDUPE_INVOCATION_LOG"
if [ "$*" = "dedupe scan ." ]; then
  exit "${DEDUPE_EXIT:-0}"
fi
SH
chmod +x "$tmp_dir/standardsctl"

cat >"$tmp_dir/node" <<'SH'
#!/bin/sh
exit 0
SH
chmod +x "$tmp_dir/node"

export PATH="$tmp_dir:$PATH"
export DEDUPE_INVOCATION_LOG="$tmp_dir/invocations.log"
unset MAKEFLAGS

DEDUPE_EXIT=0 make --no-print-directory -C "$repo_root" verify-all >/dev/null
expected_invocations=$'audit\ncompile-context --verify\nhiss coverage --verify\ndedupe scan .'
actual_invocations="$(<"$DEDUPE_INVOCATION_LOG")"
if [[ "$actual_invocations" != "$expected_invocations" ]]; then
  report_failure "make verify-all did not run the complete governance sequence"
fi

: >"$DEDUPE_INVOCATION_LOG"
if DEDUPE_EXIT=23 make --no-print-directory -C "$repo_root" verify-all \
  >"$tmp_dir/failing.stdout" 2>"$tmp_dir/failing.stderr"; then
  report_failure "make verify-all ignored a failing clone scan"
fi
if [[ "$(tail -n 1 "$DEDUPE_INVOCATION_LOG")" != "dedupe scan ." ]]; then
  report_failure "failing fixture did not reach the clone scan"
fi

[[ "$fail" -eq 0 ]]
