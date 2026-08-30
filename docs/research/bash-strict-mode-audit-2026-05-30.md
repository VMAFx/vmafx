<!-- markdownlint-disable MD013 MD060 -->
# Research digest: bash strict-mode + trap-cleanup audit (2026-05-30)

## Scope

Audit all 59 `*.sh` files under `scripts/`, `dev/scripts/`, `tools/`,
and `.github/` for adherence to the four bash hygiene invariants:

1. First non-shebang executable line is `set -euo pipefail` (or POSIX
   equivalent for `#!/usr/bin/env sh`).
2. `IFS=$'\n\t'` set when iterating over filenames or newline-
   delimited output.
3. `trap '...' EXIT` (and ideally `INT TERM`) for any `mktemp` /
   `mktemp -d` allocation.
4. `LC_ALL=C` for `sort` / `awk` / `grep` operations whose ordering
   feeds into a downstream correctness check (ADR-numbering,
   dispatch-registry diffs, etc.).

## Methodology

```bash
find scripts dev/scripts docker .github tools -name '*.sh' \
  -not -path '*/node_modules/*' 2>/dev/null > /tmp/all_sh.txt
# Per-file scan: first 80 lines for `set -[eu]` and `pipefail`;
# full file for `mktemp` and `trap ... EXIT`; full file for
# `| sort` vs `LC_ALL=C`.
```

Avoided overlap with three in-flight PRs:

- PR #318 (scripts hygiene trap + stderr): `scripts/perf/bench-multi-resolution.sh`, `scripts/release/concat-changelog-fragments.sh`.
- PR #350 (shell-injection round 2): `dev/scripts/dev-mcp-entrypoint.sh`, `scripts/ci/sycl-bench-env.sh`.
- PR #378 (.claude/skills audit): out of scope (skills tree, not shell).

## Findings

| Severity | Count | Files |
|---|---|---|
| HIGH — no strict mode at all | 1 | `scripts/run_unittests.sh` |
| HIGH — sourced helper needs explicit doc | 1 | `tools/ensemble-training-kit/_platform_detect.sh` |
| MEDIUM — `set -eu` but missing `pipefail` | 2 | `scripts/ci/check-agent-worktree-drift.sh`, `scripts/ci/test_check_agent_worktree_drift.sh` |
| MEDIUM — `mktemp` without script-wide trap | 2 | `scripts/ai/fetch-tiny-blobs.sh`, `dev/scripts/smoke-probe-loop.sh` |
| MEDIUM — `sort` over filename / numeric input without `LC_ALL=C` | 3 | `scripts/ci/check-adr-numbering.sh`, `scripts/ci/check-dispatch-registry.sh`, `scripts/adr/next-free.sh` |

The remaining 50 scripts pass all four invariants.

## Fixes applied

Per-file minimal changes (no behaviour change for happy paths):

1. **`scripts/run_unittests.sh`** — promoted to POSIX strict mode
   (`set -eu` + guarded `pipefail` opt-in for non-dash hosts);
   added `${1:-}` to silence `-u` on empty `$1`.
2. **`tools/ensemble-training-kit/_platform_detect.sh`** — added an
   inline comment explaining why no `set -euo pipefail` at top
   level: sourcing the file would mutate the caller's shell
   options. Callers (`01-prereqs.sh` etc.) each set their own.
3. **`scripts/ci/check-agent-worktree-drift.sh` + test** — `set -eu`
   → `set -euo pipefail`.
4. **`scripts/ai/fetch-tiny-blobs.sh`** — added `IFS=$'\n\t'` +
   `_TINY_STAGING_FILES` array + `trap _cleanup_staging EXIT INT
   TERM`. `fetch_one` now appends to the array on `mktemp` and
   pops on success / failure so the trap fires only on signal /
   abort, not on the normal path.
5. **`dev/scripts/smoke-probe-loop.sh`** — same pattern with
   `_SMOKE_TMPFILES`. The smoke-probe runs as PID 1 in a long-
   lived container; SIGTERM at container stop now sweeps the
   per-iteration `mktemp` outputs.
6. **`scripts/ci/check-adr-numbering.sh`**,
   **`scripts/ci/check-dispatch-registry.sh`**,
   **`scripts/adr/next-free.sh`** — `LC_ALL=C` prefixed on each
   `sort` / `sort -u` whose output feeds into a numeric-prefix
   collision check (ADR numbers, dispatch-registry symbols).

## Verification

```bash
# Syntax check (sh -n for POSIX, bash -n for bash):
for f in <9 files>; do bash -n "$f"; done   # all OK

# Project shfmt config:
shfmt -d -i 2 -ci <9 files>                  # no diff

# Self-tests for fixed scripts:
bash scripts/adr/test-next-free.sh                              # 12 passed, 0 failed
bash scripts/ci/test_check_agent_worktree_drift.sh              # OK
bash tools/ensemble-training-kit/tests/test_platform_detect.sh  # 16 assertions passed
```

shellcheck was not run because it is not installed on the dev
container or in CI (verified: `which shellcheck` returns
not-found). Queued as a follow-up: land shellcheck in `make lint`

- CI gate to prevent regression.

## Reproducer

```bash
# Run the audit script:
python3 <<'PY'
import re
files = open('/tmp/all_sh.txt').read().split()
for f in files:
    content = open(f).read()
    has_e = bool(re.search(r'set\s+-[a-zA-Z]*e', content[:600]))
    has_u = bool(re.search(r'set\s+-[a-zA-Z]*u', content[:600]))
    has_pf = bool(re.search(r'pipefail', content[:600]))
    has_mktemp = bool(re.search(r'\bmktemp\b', content))
    has_trap = bool(re.search(r'\btrap\b.*\bEXIT\b', content))
    issues = []
    if not has_e: issues.append('E')
    if not has_u: issues.append('U')
    if not has_pf: issues.append('PF')
    if has_mktemp and not has_trap: issues.append('TRAP')
    if issues: print(f"{f}: {','.join(issues)}")
PY
```

Expected output on `fix/bash-strict-mode-sweep` tip: empty.
