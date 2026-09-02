<!-- markdownlint-disable MD060 -->
# ADR-0899: Bash strict-mode + trap-cleanup sweep across in-tree shell scripts

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude
- **Tags**: `ci`, `agents`, `shell`, `hygiene`

## Context

The fork ships 59 `*.sh` files under `scripts/`, `dev/scripts/`,
`tools/`, and `.claude/` covering pre-commit gates, ADR allocator,
CI gates, smoke-probe loops, ensemble training, and dev container
helpers. Two recent PRs (#318 scripts-hygiene-trap, #350 shell-
injection round 2) closed adjacent classes of bugs but left a tail
of files whose first non-shebang line was a naked statement instead
of `set -euo pipefail`. The symptoms of the residual gap:

- `scripts/run_unittests.sh` (`#!/usr/bin/env sh`) ran with neither
  `-e` nor `-u`. A missing argument silently fell through to a
  pattern that matched zero tests; a failing `python3 -m unittest`
  exited 0 because nothing checked `$?`.
- `dev/scripts/smoke-probe-loop.sh` allocated a `mktemp` per backend
  per probe iteration inside `probe_backend`. SIGTERM mid-probe
  (container stop, OOM kill) left `tmp.XXXXXX` orphans in `$TMPDIR`
  with no script-wide trap to sweep them.
- `scripts/ai/fetch-tiny-blobs.sh` did the same with a `dest.XXXXXX`
  stage file next to the destination `.onnx` blob — Ctrl-C in the
  middle of a curl left a partial blob with an unpredictable name.
- `scripts/ci/check-agent-worktree-drift.sh` (a pre-commit gate) and
  its self-test ran with `set -eu` but no `pipefail`. A `git rev-parse
  | grep` pipeline that fell through silently was the symptom shape.
- `scripts/adr/next-free.sh` and several ADR-numbering / dispatch-
  registry consumers used unlocked `sort` / `sort -u` on filename
  numerics. Under a non-C locale (German de_DE.UTF-8 on the dev box;
  C.UTF-8 in CI containers) `sort` collation can reorder digits
  vs hyphens unpredictably across hosts, producing different "next
  free" answers on identical state.

These are individually small bugs. As a class they are the same kind
of footgun: every script needs `set -euo pipefail` + `IFS=$'\n\t'`
at the top, every `mktemp` needs an EXIT trap, every sort over
filename / numeric input needs `LC_ALL=C`. The fork's `AGENTS.md`
already documents the pattern; this sweep enforces it.

## Decision

Apply a strict-mode + trap-cleanup + locale-stable-sort sweep across
the 9 in-tree shell scripts where the audit found a real gap (out of
59 total scanned). The fix is per-file and minimal:

1. Promote `set -eu` to `set -euo pipefail` where pipefail was
   missing (2 CI gates).
2. Add `set -euo pipefail` (or POSIX-sh equivalent with a guarded
   pipefail opt-in) where strict mode was entirely absent
   (`run_unittests.sh`).
3. Add `trap _cleanup EXIT INT TERM` patterns where `mktemp` was
   used without one (`smoke-probe-loop.sh`, `fetch-tiny-blobs.sh`),
   tracking in-flight staging files in a script-scope array.
4. Add `LC_ALL=C` to every `sort` / `sort -u` over filenames or
   numeric prefixes (`check-adr-numbering.sh`,
   `check-dispatch-registry.sh`, `next-free.sh`).
5. Document `_platform_detect.sh` as deliberately sourced-without-
   strict-mode (would clobber the caller's shell options).

Behaviour-preserving changes only: every modified script's existing
self-test (`test-next-free.sh`, `test_check_agent_worktree_drift.sh`,
`test_platform_detect.sh`) continues to pass unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Top-of-script `set -euo pipefail` enforced via shellcheck CI gate | Mechanical; no per-file judgement needed | Existing PR #318 already partially landed it; conflicts with sourced files (`_platform_detect.sh`) where strict mode would mutate caller state | Picked targeted fixes for the 9 actual offenders + an inline rationale on the sourced exception; a blanket shellcheck SC2148/SC2154 promotion can follow in a separate PR |
| Wait until shellcheck is installed in CI and let it flag every file in one mass-rewrite | Single PR | Shellcheck isn't currently in CI (`which shellcheck` returns not-found); blocking on tool install delays a tractable cleanup | Ship the surgical fixes now and queue the shellcheck-in-CI follow-up as a separate ADR |
| Rewrite sourced helpers to define functions only and assume callers handle strict mode | Already the case | No behavioural change | This is the actual decision for `_platform_detect.sh`; the new comment makes it explicit |

## Consequences

- **Positive**: `mktemp` leaks during SIGTERM no longer accumulate
  in `$TMPDIR`; `sort` ordering is now host-locale-independent; two
  CI gates upgraded from partial to full strict mode; sourced helper
  carries inline doc explaining why it's an exception.
- **Negative**: One extra `IFS=$'\n\t'` to read; trap arrays add ~10
  lines per script. Negligible.
- **Neutral / follow-ups**: A future PR can land `shellcheck` in the
  `make lint` target + CI gate to prevent regression. Out of scope
  here because shellcheck is not installed on the dev container.

## References

- ADR-0332: Agent worktree-drift hard guard (one of the gates fixed)
- ADR-0386 / ADR-0535: ADR-numbering atomic allocator (locale-stable
  sort fix is for this code path)
- PR #318: scripts-hygiene-sweep (trap + stderr) — predecessor that
  closed the perf / changelog scripts; this PR closes the remaining
  9 files
- PR #350: shell-injection round 2 — predecessor that closed the
  dev-mcp-entrypoint + sycl-bench-env scripts
- Source: agent task brief 2026-05-30 ("audit all shell scripts for
  strict-mode adherence, trap-on-EXIT cleanup, and IFS hygiene")
