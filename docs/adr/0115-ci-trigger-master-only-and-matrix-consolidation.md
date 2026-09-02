<!-- markdownlint-disable MD013 -->
# ADR-0115: CI workflows trigger on `master` only; consolidate windows.yml into libvmaf.yml

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, github, build, framework

## Context

Audit of `.github/workflows/*.yml` triggered by user observation that the
displayed PR check `build (MINGW64, mingw-w64-x86_64)` looked
"windows-only" surfaced two real defects, not just a naming illusion.

**Defect 1 — silent trigger gap.** Three workflows (`docker.yml`,
`ffmpeg.yml`, `libvmaf.yml`) trigger only on
`pull_request: branches: [sycl]`. The fork's GitHub default branch is
`master` (per `gh repo view --json defaultBranchRef.name → master`)
and PRs increasingly target `master` directly (e.g. PR #46). Any PR
that targets `master` silently skips those three workflows. In
particular, `libvmaf.yml` carries the entire Linux/macOS/ARM
build-and-test matrix — `Ubuntu gcc`, `Ubuntu clang`, `macOS clang`,
`Ubuntu ARM clang`, `Ubuntu SYCL`, `Ubuntu CUDA`, `Ubuntu SYCL+CUDA`,
plus a separate `Windows MinGW64` job that nobody had noticed
because the workflow itself doesn't run.

`git log master..sycl` is empty — the `sycl` branch carries no
commits ahead of `master`. It was the previous fork-default branch
and is effectively dead, but the workflow trigger lists were never
updated when the default flipped.

**Defect 2 — duplicate Windows job.** `libvmaf.yml` already contained
a fully-formed `windows:` job (`name: Windows MinGW64`, runs-on
`windows-latest`, MSYS2 / MINGW64 setup) that has been silently dead
since the trigger gap was introduced. A separate `windows.yml`
workflow shipped a near-identical job with slightly *better* config
— `concurrency: cancel-in-progress: true`, `timeout-minutes: 45`,
explicit `-static -mthreads` link flags, `if-no-files-found: error`
on the artifact upload — and was the only Windows check actually
running on PRs.

The duplication is pure waste: same MSYS2 install, same meson
configure, same artifact upload, both keyed on `master`-ish branch
state, neither aware of the other.

User direction (popup): "Fix all three silently-dead workflows +
consolidate windows" and "Drop sycl, use `[master]` only".

## Decision

Two coordinated edits, no production-code change:

1. **All `pull_request` / `push` triggers across the workflow
   directory drop the `sycl` branch.** Every workflow that previously
   said `branches: [sycl]` or `branches: [sycl, master]` becomes
   `branches: [master]`. Six files changed: `ci.yml`, `docker.yml`,
   `ffmpeg.yml`, `libvmaf.yml`, `lint.yml`, `security.yml`. (The
   `master`-only trio — `docs.yml`, `release-please.yml`,
   `scorecard.yml` — was already correct.)

2. **`windows.yml` is deleted; its content is merged into
   `libvmaf.yml`'s existing `windows:` job.** The merged job inherits
   the better config from `windows.yml` (concurrency,
   timeout-minutes, static linking flags, error-if-no-files-found
   artifact). `libvmaf.yml` also gains a top-level `concurrency:`
   block so a force-push doesn't queue duplicate matrix runs.

**Required-status name preservation.** `build (MINGW64,
mingw-w64-x86_64)` is one of 19 required status checks on the
`master` branch protection rule. GitHub renders matrix-job check
names as `${job-key-or-name} (${matrix.value1}, ${matrix.value2})`.
The deleted windows.yml used `jobs: build:` with no `name:` — so
the required check is literally `build (...)`. To preserve that
name across the consolidation, the merged job in `libvmaf.yml`
keeps the bare key `build:` (no `name:` override). Renaming would
require a synchronised branch-protection update; preserving the
name keeps the change atomic.

## Alternatives considered

1. **Only fix `libvmaf.yml`'s trigger; consolidate Windows; leave
   `docker.yml` + `ffmpeg.yml` broken.** Matches the original ask
   exactly. But the same root cause (sycl→master default flip without
   workflow trigger updates) keeps biting on every later PR until
   it's fixed across the board. Deferring it just turns one ADR into
   two.
2. **Keep the `[sycl, master]` dual-branch trigger pattern and
   change nothing about `sycl`.** Harmless if `sycl` stays empty
   (no commits to fire on), useful if anyone resurrects it. But
   four workflows already do this and three don't — the
   inconsistency confuses readers and audits, and the `sycl` branch
   is verifiably empty *now*. User explicitly chose `[master]` only.
   If `sycl` ever needs CI again, re-adding it to the trigger list
   is a one-line edit.
3. **Keep both workflow files; only fix triggers.** Smallest
   possible change — six trigger edits, no consolidation. But
   `libvmaf.yml` would then have a `windows:` job that runs *and* a
   separate `windows.yml` workflow whose `build:` job runs the
   *same* MSYS2/MINGW64 build. Two PR checks, doubled MSYS2 cache
   pressure, twice the runner-minute budget. Net cost of the
   consolidation is one rename (`Windows MinGW64` → `build`); the
   cost of *not* consolidating is permanent.
4. **Rename the merged job to something descriptive (`Windows
   MinGW64`) and update branch protection's required-checks list
   in the same change.** Cleaner long-term name. But branch
   protection edits aren't atomic with PR merges — there's a
   window between "PR merged, name changed" and "protection
   updated, new name required" where every other PR's required
   check is missing and blocks. Preserving `build` is the
   atomic-merge-safe choice. A future cleanup ADR can rename and
   coordinate the protection update.
5. **Delete the empty `sycl` branch on the remote.** Out of scope —
   destructive and not asked. The branch can sit there at its
   current (behind-master) commit. Trigger removal alone neutralises
   it for CI purposes. If we ever want to delete it, that's its own
   decision and its own ADR.

## Consequences

**Positive:**

- Every PR that targets `master` now runs the full build matrix
  (Ubuntu gcc/clang/ARM, macOS, SYCL, CUDA, SYCL+CUDA, Windows
  MinGW64). PR #46 was the first PR to surface this gap; future PRs
  inherit the fix.
- One workflow file owns the build matrix. New matrix entries
  (windows-2022, macos-14 ARM, etc.) land in one place with one
  trigger config and one concurrency group.
- No required-status-check rewiring needed — the `build (MINGW64,
  mingw-w64-x86_64)` check name is preserved exactly.
- `concurrency: cancel-in-progress: true` on `libvmaf.yml` saves
  runner minutes when force-pushes supersede in-flight matrix runs.

**Negative:**

- The merged Windows job now runs as part of `libvmaf.yml` instead
  of as a standalone workflow. If `libvmaf.yml` itself fails at the
  workflow-setup stage (e.g. `concurrency` key syntax error) the
  Windows job is collateral. Mitigation: the `concurrency:` block
  is well-formed YAML and validated locally before push.
- Anybody bookmarking the URL of the old `windows.yml` runs sees a
  404. GitHub Actions retains historical run pages, but the
  workflow-overview page for `windows` disappears. Not user-facing.
- The `build` job key in `libvmaf.yml` is unintuitive next to its
  more descriptive sibling `libvmaf-build`. The header comment
  flags this and points at this ADR. A future rename — paired with
  a branch-protection edit — can fix the cosmetics.

**Neutral:**

- The empty `sycl` branch is unaffected. If anyone resurrects it,
  re-adding `sycl` to the trigger lists is a six-file one-line
  edit.
- No production code changes. Build-output / install-tree /
  CLI / public API is identical.

## References

- [ADR-0024](0024-netflix-golden-preserved.md) — Netflix golden
  gate on `ci.yml` (also a required status check; trigger update
  here doesn't affect it because `ci.yml` already had `master` in
  its trigger list).
- [ADR-0037](0037-master-branch-protection.md) — `master` branch
  protection enforces 19 required status checks including
  `build (MINGW64, mingw-w64-x86_64)`. The
  required-name-preservation decision above is the entire reason
  the merged job uses `build:` as its key instead of something
  more descriptive.
- `req` (paraphrased): user noticed during PR #46 watch that the
  matrix build check was rendered "windows-only" in the displayed
  PR check list; on the popup, selected "Fix all three
  silently-dead workflows + consolidate windows" + "Drop sycl, use
  `[master]` only" + "Separate PR after #46 merges, but do it
  ASAP".
- Per-surface doc impact: GitHub Actions workflow definitions are
  themselves the user-discoverable surface for CI. The header
  comment in `libvmaf.yml`'s merged `build:` job documents the
  required-status-name-preservation invariant inline; this ADR
  documents the `sycl` removal and the consolidation rationale.
  No `docs/development/` page change required — there is no
  general "CI workflows" doc to update.
