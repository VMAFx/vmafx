<!-- markdownlint-disable MD013 MD060 -->
# Research-1346: Building native release artifacts on a hosted runner inside the libvmaf-build stage

- **Status**: Active
- **Workstream**: [ADR-1346](../adr/1346-hosted-slim-container-release-build.md), supersedes [ADR-1178](../adr/1178-dev-container-image-publish.md)
- **Last updated**: 2026-09-27

## Question

Can `build-artifacts` in `.github/workflows/supply-chain.yml` build the
native Linux bundle on a GitHub-hosted runner inside an image made from
`dev/Containerfile`, reliably within one job? If so, which image-build
mechanism, cache policy and timeout should it use? And what does a
container-built bundle require from the systems that run it?

## Sources

- `gh run list` / `gh run view --json jobs` for
  `.github/workflows/dev-container-build.yml` (the ADR-0819 PR gate) and
  `.github/workflows/dev-container-publish.yml`, queried 2026-09-27.
- `gh run view --log` for publisher runs 35868459269 (success),
  36273853106, 35861745414, 35831873445, 35817279976, 35798423831 and
  35450966064 (cancelled).
- GitHub Actions dependency caching reference:
  <https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching>.
- Local runs of `scripts/release/build-native-release-artifacts.sh` in a
  `libvmaf-build` image, and of `scripts/release/verify-native-release-artifacts.sh`
  in digest-pinned `ubuntu:24.04` and `ubuntu:26.04` containers.

## Findings

### Build time on hosted runners

| Job | Mechanism | Duration | Outcome |
|---|---|---|---|
| `Dev Container Build work` (PR gate), the 14 runs that did work between 2026-09-25 and 2026-09-26 | `docker build --target libvmaf-build`, default builder, no cache, no push | build step 28.0 / 34.5 / 35.1 min (min / median / max) | all succeeded |
| `dev-container-publish.yml`, last 10 runs | `docker/build-push-action`, `cache-from`/`cache-to: type=gha,mode=max`, push | successful build-and-push steps 50.5, 56.6 and 58.9 min | 3 succeeded, 6 cancelled at the 60-minute limit, 1 failed after 5 minutes |

The publisher's successful run 35868459269 breaks down as follows. The
BuildKit solve ran from 13:39 to 14:11 (about 32 minutes). Layer export
(compression) took 499 s, and the push took 47 s. The GitHub Actions cache
export then ran from 14:19 to 14:29.

All six cancelled runs had already pushed the image, both the
`sha-<commit>` and `:master` tags, before they were cancelled. Each was
cancelled 16 to 18 minutes into "exporting to GitHub Actions Cache", so the
cosign signing step that follows never ran.

The cache also saved little on the build side. With `cache-from` restored, the
two heaviest RUN steps still executed for 515 and 489 seconds. The repository
cache budget is 10 GB, while the stage's layer set is about 29.5 GB (measured
in the ADR-1178 digest), so most layers cannot stay cached.

### Cache scope for a tag-triggered run

GitHub documents that a run can restore caches from its current ref and from
the default branch. Pull-request caches are scoped to `refs/pull/N/merge`, so
other refs cannot restore them, and a run cannot restore a cache created for a
different tag name. A release run could therefore only read caches that the
tag itself or `master` wrote, so pull requests cannot poison it. The cache is
still left out: it buys almost nothing, as shown above, and a cache-free build
makes the release depend only on the tagged tree and on the upstream sources
the Containerfile pins.

### Behaviour of the release build inside the stage

The release build ran in `vmaf-libvmaf-build:adr1311`, a 2-day-old local build
of the same stage, as the invoking UID with the checkout mounted.

- `cc` is GCC 15.2.0 and Meson is 1.10.1. Meson wraps the compiler in
  `/usr/bin/ccache` automatically.
- Meson found `libonnxruntime` 1.30.0 in the image, and the `enable_dnn=auto`
  feature turned on the DNN backend. `libvmaf.so` then NEEDED
  `libonnxruntime.so.1`, and the CLI's RUNPATH gained `/usr/local/lib`.
  Neither is true of a build on a bare runner. With `-Denable_dnn=disabled`,
  `libvmaf.so` NEEDs only `libstdc++`, `libm`, `libgcc_s` and `libc`, and the
  CLI's RUNPATH is `$ORIGIN/../src` again.
- `libvmaf.so` binds `sqrtf@GLIBC_2.43` and `GLIBCXX_3.4.30`; the CLI's newest
  glibc symbol is `GLIBC_2.38`.
- `verify-native-release-artifacts.sh` passes in the container and in
  `ubuntu:26.04` (glibc 2.43). In `ubuntu:24.04` (glibc 2.39, the current
  `ubuntu-latest` image) it fails with
  `version 'GLIBC_2.43' not found (required by .../libvmaf.so.3)`.
- The build fetches nothing from the network, since no Meson wrap or
  subproject downloads appear in the log. `docker run --network none` is
  therefore safe.

### End-to-end proof on a freshly built stage

`scripts/ci/build-dev-container-stage.sh` built the stage from this change's
exact tree in 1911 s on the maintainer workstation. Pulling the ROCm base image
took 1597 s of that. The compile steps reused the workstation's BuildKit ccache
mounts, so this time says nothing about hosted runners; the PR gate figures
above do. The release step then ran as the workflow runs it (`docker run
--network none --user "$(id -u):$(id -g)"`, checkout mounted, capped at four
CPUs) and finished in 53 to 61 s. It staged the SONAME chain, the CLI,
`models.tar.gz` and a stamp with `image_title=vmaf-dev-mcp`. The stamp verified
on the host. Two builds from separate clones produced byte-identical
`libvmaf.so`, `vmaf` and `models.tar.gz`. The runtime check behaved as in the
earlier image: `ubuntu:26.04` passed and `ubuntu:24.04` failed on
`GLIBC_2.43`.

### Version string in a tag checkout

`core/include/meson.build` derives `VMAF_VERSION` from
`git describe --tags --long --match 'v*.*.*'` and falls back to the Meson
project version only when no tag is reachable. `actions/checkout`, given an
unqualified tag `ref`, fetches `+refs/tags/<tag>*:refs/tags/<tag>*` even at
depth 1 (`src/ref-helper.ts` at the pinned commit). The release checkout
therefore reaches the tag. We reproduced that checkout shape locally with a
scratch `v3.2.1` tag on this commit. `vmaf --version` printed
`v3.2.1-0-g44533b1`, and `verify-native-release-artifacts.sh ... 3.2.1` failed
with `staged vmaf reported 'v3.2.1-0-g44533b1', expected '3.2.1'`. The
verifier's exact comparison and the build's `--long` describe disagree for
every tagged build, whichever runner does the build. The proofs above used
tagless shallow clones, which take the fallback version.

### Identity carried by the stage

`dev/Containerfile` writes `/etc/vmafx-dev-container` once, in `build-deps`,
with `image_title=vmaf-dev-mcp`. `libvmaf-build`, `go-build` and `dev-mcp`
inherit it unchanged, and so does `dev/Containerfile.runner`, which is built
from `vmaf-dev-mcp:local`. The `vmaf-sycl-arc-runner` title that ADR-1178
added to the gate's allowlist was never written by any image.

## Alternatives explored

- **Bring the Arc runner up**: rejected by the maintainer's popup answer. See
  the ADR's References.
- **Hosted runner, no container**: breaks ADR-1102.
- **Pull the GHCR image with a job-level `container:`**: the publisher that
  feeds it times out, and the image would not come from the tag.
- **Build-push-action with a `gha` cache**: 51 to 59 minutes when it
  succeeds, and six of the last ten runs were cancelled at 60 minutes.

## Open questions

- Release blocker, independent of the runner. In a tag checkout
  `vmaf --version` prints `vX.Y.Z-0-g<sha>`, so the verifier's exact
  comparison fails `build-artifacts`. Either the verifier accepts exactly the
  `v<version>-0-g<hex>` form, or the build emits the plain version on an exact
  tag. The verifier also still rejects `-rc.N` versions.
- The glibc 2.43 floor limits the native bundle to Ubuntu 26.04-class
  systems. Supporting older distributions would need a build image with an
  older glibc, which is a separate decision against ADR-1102's single
  container.
- Ubuntu archive packages in the stage (compilers, Meson) resolve when the
  stage is built. Rebuilding an old tag later may therefore use newer
  distribution packages.
- `dev-container-publish.yml` keeps timing out during cache export. Dropping
  `cache-to` (and `cache-from`) or raising `timeout-minutes` would stop the
  cancellations and let the signing step run.

## Related

- ADRs: [ADR-1346](../adr/1346-hosted-slim-container-release-build.md), [ADR-1178](../adr/1178-dev-container-image-publish.md), [ADR-1102](../adr/1102-phase4b9-container-only-publishing.md), [ADR-0819](../adr/0819-dev-container-ci-gate.md)
- Prior digest: [Research for ADR-1178](1178-dev-container-image-publish.md)
- PRs: release PR #1213 (v1.0.0-rc.1)
