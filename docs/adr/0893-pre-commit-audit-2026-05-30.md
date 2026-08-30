<!-- markdownlint-disable MD060 -->
# ADR-0893: Pre-commit config audit — 2026-05-30

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: ci, lint, hygiene, pre-commit

## Context

`.pre-commit-config.yaml` had not been audited since the last
formatter / linter bumps several months ago. Three drift classes had
accumulated:

1. **Stale tool revisions** — `isort` was pinned at `5.13.2` (released
   2023-12-13, ~17 months old at audit time), `ruff-pre-commit` at
   `v0.15.13` (two patch versions behind `v0.15.15`).
2. **Missing belt-and-suspenders hooks** — the audit walked the
   `pre-commit-hooks` repo's hygiene catalogue and confirmed five of
   the requested six guards (`check-yaml`, `end-of-file-fixer`,
   `mixed-line-ending`, `check-merge-conflict`, `detect-private-key`)
   were already present, but `forbid-new-submodules` was missing — a
   supply-chain gap, since the fork pulls upstream via
   `subprojects/` (Meson wraps) and `ffmpeg-patches/` (out-of-tree
   patches), never via `.gitmodules`. A stray submodule entry would
   bypass the wrap pinning machinery entirely.
3. **Autoupdate noise** — `pre-commit autoupdate` proposed downgrading
   `gitleaks` from `v8.30.1` (current) to `v8.30.0`. Verified via
   `git ls-remote --tags` that `v8.30.1` is the latest upstream tag;
   the autoupdate heuristic mis-ranks tag sort order on releases
   that land out of branch order. Skipped.

`pre-commit run semgrep-local --all-files` was failing with
`exit code 2` (io_uring / RLIMIT_MEMLOCK exhaustion under
pre-commit's per-CPU fan-out). That failure is **pre-existing on
master** and is the subject of the in-flight PR #340 (ADR-0867,
adds `require_serial: true` to the `semgrep-local` hook). Outside
this audit's scope; this PR does not touch the `semgrep-local` block
to avoid a merge conflict with #340.

PR #342 (ADR-0866) is in flight and inserts a new `markdownlint-cli2`
repo block between `shellcheck-py` and `gitleaks`. This audit
intentionally does not touch that file region either.

## Decision

Apply the following deltas to `.pre-commit-config.yaml`:

1. **Add `forbid-new-submodules`** under the
   `pre-commit/pre-commit-hooks` block, with a comment explaining
   the fork's wraps-and-patches dependency posture.
2. **Bump `isort`** from `5.13.2` to `6.0.1` (latest stable; `profile
   = "black"` semantics unchanged in 6.x; one latent import-grouping
   fix surfaced in `tools/vmaf-tune/tests/test_codec_adapter_av1_videotoolbox.py`
   and is applied in the same PR per the touched-file rule).
3. **Bump `ruff-pre-commit`** from `v0.15.13` to `v0.15.15` (two
   patch versions; no rule changes affect the fork's `[tool.ruff]`
   selection set).
4. **Keep `gitleaks` at `v8.30.1`** — autoupdate's suggested
   downgrade to `v8.30.0` was rejected after verification against
   the upstream tag list.

Other hooks (`clang-format` v22.1.5, `black` 26.5.1, `shfmt` v3.13.1-1,
`shellcheck` v0.11.0.1, `conventional-pre-commit` v4.4.0) are
already on the latest upstream revisions and untouched.

Larger-jump options (e.g. `isort` 7.x / 8.x, `ruff` 0.16.x betas)
were deferred — the touched-file lint-clean rule (CLAUDE.md §12 r12)
makes the gate increasingly strict over time, so each bump needs
its own diff-with-blame audit. The 6.0.1 step is the safe one.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Apply `pre-commit autoupdate` verbatim | One-command, zero thought. | Would downgrade gitleaks (v8.30.1 → v8.30.0); would jump isort to `9.0.0a3` (alpha pre-release); skips the missing-hook audit entirely. | Autoupdate's tag-sort heuristic produces wrong answers on repos that release point releases out of branch order; alpha pre-releases violate the project's "stable upstream pins only" hygiene. |
| Bump isort to 8.0.1 (latest stable) | Latest features, Python 3.13 support. | 8.x dropped Python 3.8 (we're on 3.14 so OK), but 7.x→8.x changelog calls out several `profile = "black"` interaction tweaks; needs a wider audit than this audit is sized for. | Conservatism — 6.0.1 has been the de facto stable line for ~18 months, ships the Python 3.13 fix we care about, and keeps the diff small. |
| Add `markdownlint-cli2` here too | Single audit PR covers all hygiene. | Direct conflict with PR #342 (already in flight, ADR-0866). | Coordination — one PR per moving config block. |
| Add `require_serial: true` to `semgrep-local` here | Single audit PR closes the io_uring bug. | Direct conflict with PR #340 (already in flight, ADR-0867). | Same coordination argument. |
| Skip `forbid-new-submodules` (the fork has no submodules today) | One fewer hook to maintain. | Defense-in-depth: an accidental `git submodule add` (e.g. an agent following stale instructions) would bypass the `subprojects/` wrap pinning and the SBOM machinery entirely. The hook costs ~0 ms at commit time. | Cheap insurance against a real failure mode. |

## Consequences

- **Positive**:
  - Submodule additions are now caught at commit time, not at code
    review or post-merge supply-chain scan.
  - isort 6.0.1 + ruff 0.15.15 close two minor drift windows
    against the upstream ecosystem.
  - Documents the audit cadence — next audit on a similar drift
    threshold (~6 months or when CI starts surfacing
    deprecation warnings on any pinned hook).
- **Negative**:
  - One latent isort fix lands in this PR's diff
    (`tools/vmaf-tune/tests/test_codec_adapter_av1_videotoolbox.py`).
    Acceptable per the touched-file rule.
- **Neutral / follow-ups**:
  - Re-audit after PR #340 + PR #342 land — the in-flight rebase
    deltas should be trivial (different file regions).
  - Future bumps to isort 7.x / 8.x deserve their own ADR with a
    diff-with-blame audit of the changed default rules.

## References

- [ADR-0867](0867-semgrep-local-serial-execution.md) — in-flight,
  fixes the `semgrep-local` io_uring failure; this audit defers to it.
- [ADR-0866](0866-wire-markdownlint-into-lint-pipeline.md) — in-flight,
  wires `markdownlint-cli2` into pre-commit; this audit does not touch
  that file region.
- [ADR-0278](0278-nolint-citation-closeout.md) — touched-file
  lint-clean rule (CLAUDE.md §12 r12) makes each gate strictness
  ratchet meaningful.
- Source: `req` (user prompt 2026-05-30 — "Audit
  `.pre-commit-config.yaml` for missing hooks, stale versions,
  broken hooks").
