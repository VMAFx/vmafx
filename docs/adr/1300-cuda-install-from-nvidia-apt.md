# ADR-1300: Install CUDA on the Linux CI legs from NVIDIA's apt repository

- **Status**: Accepted
- **Date**: 2026-09-23
- **Deciders**: Lusoris
- **Tags**: `ci`, `cuda`, `gpu`, `dependencies`

## Context

The Linux CUDA legs installed the toolkit with
`Jimver/cuda-toolkit`. That action ships its own table of downloadable CUDA
releases, so the fork could only pin a release the action already knew about.

`renovate.json` recorded the consequence as a rule:

> Jimver/cuda-toolkit's own version is NOT in this group — that is an action
> bump, not a CUDA release — but it does cap which CUDA versions the Linux legs
> can install (v0.2.36 tops out at 13.3.1), so a CUDA bump the action cannot
> serve needs the action raised first.

PR #1521 is what that rule costs. Renovate opened it for CUDA 13.4.1 and it sat
for two weeks, unmergeable in both directions: leaving `CUDA_VERSION` at 13.3.1
fails `check-cuda-pin-lockstep`, and raising it asks the action for a release it
cannot serve. Checked against the action's own source rather than its
changelog: `src/links/linux-links.ts` at v0.2.36 — its newest release, published
2026-08-02 — tops out at `13.3.1` and has no 13.4.x entry at all. The same
coupling produced `T-CI-JIMVER-CUDA-133-NOT-AVAILABLE` one version earlier.

The fork has no reason to accept that latency. NVIDIA publishes the packages
itself the day a release lands, `build-config.env` already carries
`CUDA_APT_PACKAGE` as a first-class pin, and `dev/Containerfile` already
installs from exactly that source. Only the CI legs were routed through a third
party.

## Decision

Install from NVIDIA's apt repository via `scripts/ci/install-cuda-toolkit.sh`,
which reads `CUDA_APT_PACKAGE` from `build-config.env` and installs
`cuda-nvcc-<series>` and `cuda-cudart-dev-<series>` — the same subset the
action's `sub-packages: '["nvcc", "cudart-dev"]'` selected, so runner time and
disk are unchanged. The distro tag is derived from `/etc/os-release` at run
time, because the legs span `ubuntu-latest` and `ubuntu-26.04`, and a runner
image that outruns NVIDIA's repositories fails loudly rather than installing
nothing.

Two things follow from removing the action, and both are part of this change
because ADR-1285 requires a spelling and its owner to move together:

- `renovate.json`'s CUDA manager loses the `cuda:` matchString. A manager whose
  pattern matches nothing is not harmless — it looks wired and does nothing,
  which is the failure this repository keeps re-finding.
- `CUDA_PATH_V<major>_<minor>` is taught to `check-cuda-pin-lockstep.py` as a
  sixth derived spelling. It was never covered, and it is the one case where
  the release is baked into a variable **name** rather than a value: two sites
  still read `CUDA_PATH_V13_3` after a `--write` had moved every other
  spelling to 13.4, which would have exported a 13.4 toolkit under the 13.3
  release's name.

## Alternatives considered

| Option | Why not |
| --- | --- |
| **Install from NVIDIA's apt repository** (chosen) | No third-party release table between the fork and a CUDA version; same package subset; same source the dev container already uses. |
| Wait for a `Jimver/cuda-toolkit` release carrying 13.4.x | What the branch was already doing, for two weeks, with no upstream signal. It also does not fix the next bump. |
| Switch to a different CUDA action | Trades one third party's release cadence for another's, and keeps a dependency the fork does not need. |
| Install the `cuda-toolkit-<series>` metapackage | Simpler script, but several gigabytes of runner time and disk for a build that needs `nvcc` and the cudart headers. |
| Keep the action and pin CUDA to whatever it serves | Lets a third party choose the fork's compiler version, and silently caps `ADR-0603`'s "Ubuntu 26.04 needs CUDA >= 13.2" floor from above. |

## Consequences

- **Positive**: a CUDA release can be pinned the day NVIDIA publishes it.
- **Positive**: one fewer third-party action in the build path, and the CI legs
  and the dev container now install from the same place.
- **Positive**: the lockstep gate covers a spelling it was blind to, with a
  regression case for it.
- **Negative**: the fork owns the apt bootstrap — the keyring package name and
  the `/usr/local/cuda-<x.y>` layout. Both are asserted in the script, which
  fails with a named error rather than a missing `nvcc` if either changes.
- **Negative**: the distro-tag derivation is a new coupling to
  `/etc/os-release`. A runner image NVIDIA does not serve fails the leg
  explicitly, which is the intended behaviour but is a failure the action used
  to absorb.

## References

- req: "use another action, the pr for 13.4 is there for 2 weeks and nothing is
  done" — the user, on PR #1521.
- [ADR-1285](1285-cuda-coordinated-pin-lockstep.md) — the coordinated-pin rule this
  extends with a sixth spelling.
- [ADR-0664](0664-windows-cuda-toolkit-installer.md) — the Windows legs already
  install from NVIDIA directly, for a different reason.
- [ADR-0603](0603-ubuntu-26-04-fallout-fixes.md) — the CUDA >= 13.2
  floor on Ubuntu 26.04 that the action's cap sat above.
