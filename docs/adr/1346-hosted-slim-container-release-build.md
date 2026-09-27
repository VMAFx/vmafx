<!-- markdownlint-disable MD013 MD060 -->
# ADR-1346: Build native release artifacts on a hosted runner inside the libvmaf-build container stage

- **Status**: Accepted
- **Date**: 2026-09-27
- **Deciders**: lusoris
- **Tags**: ci, release, supply-chain, container, dev-container, adr-1102, fork-local

## Context

[ADR-1102](1102-phase4b9-container-only-publishing.md) requires every published artifact to be built inside an image made from `dev/Containerfile`. [ADR-1178](1178-dev-container-image-publish.md) met that for the native Linux bundle by running `build-artifacts` in `.github/workflows/supply-chain.yml` on the self-hosted Arc A380 runner (`runs-on: [self-hosted, linux, x64, sycl-arc]`). Publishing v1.0.0-rc.1 exposed the cost of that choice. No runner with those labels is registered: the repository and organisation runner APIs both return zero runners, and the organisation runner group does not admit public repositories. A published release would therefore queue `build-artifacts` for 24 hours and cancel it. Release-event workflows run from the tagged commit, so the fix has to be on `master` before release PR #1213 merges.

ADR-1178 rejected hosted runners because a job-level `container:` pull of the ~29.5 GB `libvmaf-build` stage would not fit the disk of a hosted runner. That argument covers pulling a published image. It does not cover building the stage inside the job. The `Dev Container Build work` PR gate ([ADR-0819](0819-dev-container-ci-gate.md), `.github/workflows/dev-container-build.yml`) has built exactly that stage on `ubuntu-latest` for every container-affecting PR. In its 14 most recent executions (2026-09-25 to 2026-09-26) the build step took 28.0 to 35.1 minutes (median 34.5). The GHCR publisher, `.github/workflows/dev-container-publish.yml`, builds the same target through `docker/build-push-action` with a GitHub Actions layer cache. It is slower: the build takes about 32 minutes, layer export and push take about 9, and the `type=gha,mode=max` cache export adds 10 to 18 more. Six of its last ten runs were cancelled at its 60-minute limit. Each was cancelled while exporting the cache, after the image had been pushed, so the cosign signing step never ran.

The maintainer chose among three options by popup on 2026-09-27 (see References). The popup described the stage as small and as avoiding the 29.5 GB image. It is in fact the stage that measures about 29.5 GB. What the chosen option avoids is the registry pull and the workstation, not the size.

## Decision

`build-artifacts` runs on `ubuntu-latest`. Its steps are:

1. Check out the release tag.
2. Build the `libvmaf-build` stage of that tag's `dev/Containerfile` with `scripts/ci/build-dev-container-stage.sh`. The PR gate calls the same script, so each container-affecting PR exercises the image build that releases depend on.
3. Run `scripts/release/build-native-release-artifacts.sh` inside the built image with `docker run --network none`, as the runner's UID and GID, with the checkout mounted. The script builds with Meson, stages the libvmaf SONAME chain, the `vmaf` CLI, `models.tar.gz` and the optional u2netp mirror, stamps `container-build-provenance.txt` with `scripts/ci/check-container-build.sh --stamp`, and runs `scripts/release/verify-native-release-artifacts.sh`.
4. Hash and upload `artifacts/` on the host, unchanged, so the `hashes` output that feeds SLSA keeps the same format.

The stage build uses no layer cache and no registry. A cache restored into a tag-triggered run could come from the tag itself or from `master`, but not from pull requests, so the source is trusted. The cache still buys nothing: the repository's 10 GB cache budget cannot hold the stage's layers, and the publisher rebuilds its two heaviest steps (515 and 489 seconds) despite `cache-from`. A cache-free build also leaves no shared state for another workflow to influence. `scripts/ci/check-dev-container-build-secret.py` enforces this with a test: the shared script must not pass `--cache-from`, `--cache-to` or `--build-arg`.

The container carries tools a bare runner lacks, so the build pins two settings to keep the bundle the same as the earlier hosted build:

- **`-Denable_dnn=disabled`.** The image installs ONNX Runtime 1.30.0. Left at `auto`, the DNN option makes `libvmaf.so` NEED `libonnxruntime.so.1`, which the release does not ship.
- **`CCACHE_DISABLE=1`.** Meson otherwise wraps the compiler in the image's ccache.

The job keeps its fork and tag guard, its `release-artifacts-build` concurrency group, `permissions: contents: read` and `timeout-minutes: 90`. Ninety minutes is about 2.5 times the gate's slowest recent build.

`scripts/ci/check-container-build.sh` now accepts exactly one identity, `image_title=vmaf-dev-mcp`. Every stage of `dev/Containerfile`, including `libvmaf-build`, inherits it from `build-deps`. The retired runner title `vmaf-sycl-arc-runner` and the `vmafx-*` aliases are rejected. No image ever wrote them: `dev/Containerfile.runner` inherits the dev-container marker unchanged.

The container is Ubuntu 26.04, so `libvmaf.so` binds `sqrtf@GLIBC_2.43`. `verify-native-artifacts` therefore runs on `ubuntu-26.04`. Ubuntu 24.04, which is `ubuntu-latest` today, has glibc 2.39 and cannot load the bundle.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Bring the Arc runner up (keep ADR-1178) | No workflow change beyond a fail-fast admission probe; zero pull or build time | Every release depends on the workstation being on and the runner supervisor running; no runner is registered today; a missing runner costs a 24-hour queue | The maintainer chose the hosted option; a release should not depend on one machine |
| **Hosted runner, slim container (chosen)** | No workstation dependency; keeps ADR-1102 container provenance; reuses the PR gate's proven build; cache-free and hermetic to the tag | Adds about 35 minutes of image build to every release; raises the Linux bundle's glibc floor to 2.43 | Chosen: the only option that keeps both container provenance and hosted execution |
| Hosted runner, no container | Smallest change; the bundle would keep the runner's glibc 2.39 floor | Breaks ADR-1102: a host-built binary would be published, and the provenance stamp cannot be produced honestly | Contradicts the container-only publishing policy |
| Pull `ghcr.io/vmafx/vmafx-dev-mcp` with a job-level `container:` | No in-job image build | Pull happens before any step can free disk; the image comes from a separately scheduled workflow whose runs time out; the release's toolchain would not come from the tag | Weaker provenance and the ADR-1178 disk concern |
| Build with `docker/build-push-action` and a `gha` cache (the publisher's route) | Familiar action | 51 to 59 minutes when it succeeds, and six of its last ten runs were cancelled at 60 minutes during cache export; it also restores layers written by other runs | Slower and less hermetic than the PR gate's plain build |

## Consequences

- **Positive**:
  - Publishing a release no longer depends on the maintainer's workstation or a registered self-hosted runner.
  - Each release's toolchain comes from the tagged commit's own `dev/Containerfile`. Nothing is restored from caches or registries, and the release compile runs with networking disabled.
  - The PR gate and the release build share one stage-build script, so a Containerfile change that would break releases fails on the PR.
  - The provenance gate accepts one identity instead of four.
- **Negative**:
  - Each release run spends about 30 to 35 minutes building the stage before compiling.
  - The native Linux bundle now needs glibc 2.43 or newer (Ubuntu 26.04 or a distribution of the same generation). The bundle has never been published, so no existing user is affected. Users on older distributions use the production containers or build from source. [`docs/development/release.md`](../development/release.md) states the floor.
  - Base images and SDK versions are pinned through `build-config.env`, but Ubuntu archive packages (compilers, Meson) resolve when the stage is built. Two releases of the same tag built weeks apart can therefore use different distribution package versions.
- **Neutral / follow-ups**:
  - `dev-container-publish.yml` stays a transparency publisher and is off the release path. Its 60-minute cancellations come from the cache export, as described in Context. They do not affect releases.
  - The `sycl-arc` label now carries only the SYCL parity lane of [ADR-1177](1177-sycl-arc-self-hosted-runner.md).
  - `scripts/release/verify-native-release-artifacts.sh` accepts only `MAJOR.MINOR.PATCH` versions, so an `-rc.N` tag cannot pass this job until the verifier accepts release-candidate versions.
  - In a tag checkout, `vmaf --version` prints the `git describe --long` form (`vX.Y.Z-0-g<sha>`), which the verifier's exact comparison rejects. This fails on any runner and needs its own fix; see the research digest.

## References

- Supersedes [ADR-1178](1178-dev-container-image-publish.md).
- [ADR-1102](1102-phase4b9-container-only-publishing.md): container-only publishing policy.
- [ADR-0819](0819-dev-container-ci-gate.md): the PR gate that builds the `libvmaf-build` stage.
- [ADR-1177](1177-sycl-arc-self-hosted-runner.md): the Arc runner, which keeps its SYCL parity role.
- Research digest: [docs/research/1346-hosted-slim-container-release-build.md](../research/1346-hosted-slim-container-release-build.md).
- GitHub Actions cache access and size limits: <https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching>.
- Source: popup, 2026-09-27. Question, verbatim: "Publishing v1.0.0-rc.1 runs supply-chain.yml, whose build-artifacts job targets the self-hosted [sycl-arc] runner (ADR-1178). No runner is registered (repo and org APIs return 0), so the job would queue 24h and cancel. The fix must be on master before #1213 merges. How should the native release artifacts be built?" Options, verbatim: "Bring the Arc runner up (Recommended)" — "Keep ADR-1178. I add a hosted admission probe so a missing runner fails fast instead of queueing 24h, and write down the exact runner-start steps. Before publishing the draft, you start the ADR-1177 runner supervisor on the workstation (repo-level registration; the org runner group disallows public repos)."; "Hosted runner, slim container" — "Supersede ADR-1178 with a new ADR: build-artifacts runs on a GitHub-hosted runner inside the small libvmaf-build container stage CI already builds (ADR-0819). Keeps container provenance without the 29.5 GB image; no workstation dependency. More change before RC1."; "Hosted runner, no container" — "Build directly on a hosted Ubuntu runner. Smallest change, but it breaks ADR-1102's rule that published artifacts are built inside the canonical container." Answer, verbatim: "Hosted runner, slim container".
