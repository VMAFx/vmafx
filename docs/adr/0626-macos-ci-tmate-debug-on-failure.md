<!-- markdownlint-disable MD013 MD060 -->
# ADR-0626: SSH-into-runner debug session on macOS CI failure via tmate

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: ci, macos, debug, fork-local

## Context

Three consecutive PRs (#1355, #1403, #1412) applied static-analysis guesses
to fix a persistent SIGSEGV in the `Build — macOS clang (CPU)`,
`Build — macOS clang (CPU) + DNN`, and `Build — macOS Metal (T8-1 scaffold)`
CI legs. Each PR addressed a plausible root cause found by code inspection
(ADR-0602: `pic_cnt - 1` unsigned underflow and NULL guard gaps; ADR-0606:
seven `i >= capacity` off-by-one overreads, NaN fps division, and two
comma-placement bugs in JSON writers). After ADR-0606 the crash is still
present on macOS. There is no macOS hardware locally; every hypothesis
requires a full CI round-trip (~10 min) to test, and the CI log shows only
the signal name — not a backtrace, not a heap address, not the exact frame.

Without a real backtrace from the crashing process, further static guesses
are unlikely to converge. The cheapest path to a definitive root cause is
a live `lldb` session on the runner at the moment of failure.

## Decision

Add a single `mxschmitt/action-tmate@<sha>` step to the `libvmaf-build`
matrix job in `.github/workflows/libvmaf-build-matrix.yml`. The step is
gated by three conditions:

1. `failure()` — only fires when a preceding step has failed.
2. `runner.os == 'macOS'` — only fires on macOS legs (CPU, CPU+DNN, Metal).
3. `github.event_name == 'workflow_dispatch'` — only fires on manual
   workflow dispatch, never on PR pushes or master merges.

The `workflow_dispatch` gate is the critical safety valve: without it, every
failing PR run on a macOS leg would strand a runner for up to 30 minutes
waiting for a connection that will never come. Manual dispatch is a deliberate
operator signal.

The step uses `limit-access-to-actor: true` (restricts SSH access to the
GitHub SSH keys of the actor who triggered the dispatch) and
`connect-timeout-seconds: 1800` (unblocks after 30 minutes if nobody
connects, so the job does not hang indefinitely).

The action is pinned to commit SHA `c0afd6f790e3a5564914980036ebf83216678101`
(the commit backing the `v3` tag) per the fork's
`helpers:pinGitHubActionDigests` Renovate policy.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Continue static-analysis speculation | No runner cost | Each guess is a 10-min CI round-trip; three PRs have not converged | Diminishing returns; we need real evidence |
| Add `MALLOC_PERTURB_` + `MallocScribble` env vars to the test step | Zero runner cost; surfaces heap corruption | Still no interactive access; output only available in the job log after the crash | Adds it as a doc recommendation inside the tmate session, but not as a CI-only fix |
| Self-hosted macOS runner | Full control; no per-minute cost | Requires Apple hardware, sysadmin overhead, and GitHub Actions Runner registration; not available | Not available; operational burden exceeds the value for a debug-only capability |
| `core.debug` + verbose logging | No cost | Does not produce a backtrace; only adds more GitHub Actions log lines | Insufficient for a SIGSEGV |
| Leave macOS leg broken | No work | Blocks merge confidence; masking a real correctness bug | Unacceptable |

## Consequences

- **Positive**: Any future macOS SIGSEGV (this one or a regression) can be
  diagnosed with a real backtrace in a single dispatch run rather than
  multiple speculative PR round-trips.
- **Positive**: The `workflow_dispatch` gate makes the step a no-op on all
  normal PR pushes — zero CI cost for 100 % of regular runs.
- **Positive**: `limit-access-to-actor: true` and SHA pinning satisfy the
  fork's supply-chain and security policies.
- **Negative**: A triggered debug session consumes macOS runner minutes
  (~$0.08/min). At the 30-minute cap: ~$2.40 per session. Acceptable for
  an intentional debug action.
- **Neutral**: The step can remain in the workflow after the SIGSEGV is
  fixed; the `workflow_dispatch` gate means it is permanently inert on
  normal runs. Remove at maintainer discretion.

## References

- PR #1355 — first speculative SIGSEGV fix (ADR-0602)
- PR #1403 — second speculative fix (ADR-0606)
- PR #1412 — third speculative fix; crash persisted
- [ADR-0602](0602-macos-vmaf-write-output-segv.md) — `pic_cnt` underflow + NULL guards
- [ADR-0606](0606-macos-vmaf-write-output-segv-deep-fix.md) — off-by-one overreads + NaN fps
- [docs/development/ci-tmate-debug.md](../development/ci-tmate-debug.md) — operator guide
- req: "After 3 PRs of static-analysis guesses (#1355, #1403, #1412), we still don't know the actual macOS crash cause. Time to debug it directly."
