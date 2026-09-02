# ADR-0549: Audit cleanup bundle 2

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: `docs`, `build`, `container`, `housekeeping`, `fork-local`

## Context

A P3 audit surfaced five small, independent findings that do not each
warrant a separate ADR:

1. **Naming-01** — Eight CUDA/SYCL translation units carry an `integer_`
   filename prefix (e.g. `integer_ssim_cuda.c`) yet define `float_*`
   symbols (e.g. `vmaf_fex_float_ssim_cuda`). The `integer_` prefix is
   inherited from the Netflix upstream naming convention for the
   corresponding CPU file; the GPU twins were given the same filename for
   easy cherry-pick tracing. Renaming would diverge from upstream and
   complicate future ports. The mismatch was previously undocumented,
   creating a maintenance hazard for anyone unfamiliar with the upstream
   convention.

2. **Container-01** — `dev/Containerfile` installs three critical package
   sets (stage-1 mesa/VA-API, NEO compute-runtime, ROCm) with no
   assertion that the `apt-get install` step itself succeeded. A
   misconfigured repo, a network failure during the `.deb` fetch, or a
   package rename would produce a silently broken image that passes the
   build and fails only at container start.

3. **Docs-01** — `docs/state.md` carried a 2026-05-15 Updated note
   claiming that T-VK-T7-29-PART-2-IMPORT-NOT-IMPL and
   T-CAMBI-HIP-NOT-STARTED were added to the Open section. Both are now
   in the Recently-closed section (closed 2026-05-16 and earlier,
   respectively). The stale note misleads future readers who scan the
   Updated log for open work.

4. **Python-03** — `python/test/vmafexec_test.py:1294` carried a stale
   inline baseline comment `# 88.032956` next to a Netflix golden
   assertion. The comment refers to a score value that predates the
   current assertion baseline and was never updated; it could mislead a
   developer into editing the assertion value to match it.

5. **gitignore** — `git status` surfaced 40+ `.claude/worktrees/agent-*/`
   directories created by the parallel-agent isolation system. These are
   machine-local git worktrees and must never be committed.
   `.claude/worktrees/` was absent from `.gitignore`.

## Decision

Apply all five fixes atomically in one PR:

1. Add a one-line `/* Upstream-mirror filename: … See ADR-0549. */`
   comment to each of the 8 affected TUs, after the SPDX header, so
   the reason for the naming mismatch is visible to the next developer
   who opens the file.

2. Add a `RUN for pkg in …` verification block after each of the three
   critical `apt-get install` steps in `dev/Containerfile`. Uses
   `apt-mark showmanual` to assert the package landed in the manual set;
   emits a human-readable `FATAL:` message and exits 1 on failure so
   the build fails at the right layer instead of silently at container
   start.

3. Correct the 2026-05-15 Updated note in `docs/state.md` to reflect
   that T-VK-T7-29-PART-2-IMPORT-NOT-IMPL and T-CAMBI-HIP-NOT-STARTED
   are in Recently-closed, not Open.

4. Delete the stale `# 88.032956` inline comment on line 1294 of
   `python/test/vmafexec_test.py`. The assertion itself (`places=2`,
   `88.484423`) is untouched.

5. Add `.claude/worktrees/` to `.gitignore` so the parallel-agent
   isolation worktrees never appear as untracked files.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| One ADR per finding | Cleaner provenance per item | 5x overhead for trivial fixes | All five are one-liner changes with no design trade-off |
| Separate PRs | Smaller diff per PR | 5x CI round-trip cost | Bundling is cheaper; no ordering dependency between items |

## Consequences

- **Positive**: the eight GPU TUs now explain the naming mismatch inline;
  container build failures due to missing packages surface immediately
  during `docker build` rather than hours later at run time; `git status`
  is clean on development machines running parallel agents; the stale
  state.md note no longer misleads readers into treating closed items as
  open; the stale score comment no longer invites assertion edits.
- **Negative**: none.
- **Neutral**: the `apt-mark showmanual` check requires the package manager
  to be available at build time (always true in the Ubuntu base image).

## References

- ADR-0108: six deep-dive deliverables rule.
- ADR-0165: state.md bug-tracking rule.
- ADR-0221: changelog fragment pattern.
- ADR-0535: ADR atomic-allocator.
- `req`: per user direction, "Redo the bundle, this time pre-empting the
  lint rules."
