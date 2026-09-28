<!-- markdownlint-disable MD013 MD060 -->
# ADR-1346: Build native release artifacts on a hosted runner inside the build-deps container stage

- **Status**: Accepted (amended by [ADR-1354](1354-native-bundle-release-track.md))
- **Date**: 2026-09-27
- **Deciders**: lusoris
- **Tags**: ci, release, supply-chain, container, dev-container, adr-1102, fork-local

> **Amendment (2026-09-28, [ADR-1354](1354-native-bundle-release-track.md))**:
> The native bundle now compiles in the Debian 13 `release-build` stage of `dev/Containerfile`
> (`RELEASE_BUILDER_BASE`) instead of `build-deps`, which ends the two-track exception below and
> the release-notes checklist step. `verify-native-artifacts` runs on `ubuntu-24.04` and also
> starts the CLI on `RELEASE_RUNTIME_CC`. The release compile leaves out the unit tests
> (`-Denable_tests=false`). The hosted runner, the stage build through
> `scripts/ci/build-dev-container-stage.sh`, the `docker run` flags and the PR-gate rehearsal
> stay as decided here.

## Context

[ADR-1102](1102-phase4b9-container-only-publishing.md) requires every published artifact to be built inside an image made from `dev/Containerfile`. [ADR-1178](1178-dev-container-image-publish.md) met that for the native Linux bundle by running `build-artifacts` in `.github/workflows/supply-chain.yml` on the self-hosted Arc A380 runner (`runs-on: [self-hosted, linux, x64, sycl-arc]`). Publishing v1.0.0-rc.1 exposed the cost of that choice. No runner with those labels is registered: the repository and organisation runner APIs both return zero runners, and the organisation runner group does not admit public repositories. A published release would therefore queue `build-artifacts` for 24 hours and cancel it. Release-event workflows run from the tagged commit, so the fix has to be on `master` before release PR #1213 merges.

ADR-1178 rejected hosted runners because a job-level `container:` pull of the ~29.5 GB `libvmaf-build` stage would not fit the disk of a hosted runner. That argument covers pulling a published image. It does not cover building a stage inside the job.

The maintainer first chose a hosted runner with a "slim" container stage (popup, see References). That popup called `libvmaf-build` small, which it is not: it is the ~29.5 GB stage. Its uncached builds in the `Dev Container Build work` PR gate ([ADR-0819](0819-dev-container-ci-gate.md)) take 28 to 35 minutes, and it downloads Level Zero, `nv-codec-headers`, `vpl-gpu-rt`, SVT-AV1, VVenC, FFmpeg and ONNX Runtime at build time with no hash pins. A corrected popup (Q1) offered the stage that is actually small, `build-deps`: the digest-pinned Ubuntu 26.04 base plus Ubuntu archive packages (gcc-13, Meson, Ninja, NASM), carrying the same container marker and fetching nothing from third parties. The maintainer chose `build-deps`.

Every C/C++ stage of `dev/Containerfile` is Ubuntu 26.04 (only the `go-toolchain` stage is `golang` on Debian trixie), so the bundle binds `sqrtf@GLIBC_2.43`. `build-config.env` says published artifacts are built on the release track (`RELEASE_BUILDER_BASE`, `debian:13-slim`) and ship on `RELEASE_RUNTIME_CC` (distroless `cc-debian13`), both glibc 2.41. A bundle built in `build-deps` cannot load there, nor on Ubuntu 24.04. ADR-1178 already broke that rule. The maintainer chose (Q2) to accept the floor for RC1, record the exception here, state it in the release notes, and track a release-track build before the final 1.0.0.

## Decision

`build-artifacts` runs on `ubuntu-latest`. Its steps are:

1. Check out the release tag.
2. Build the `build-deps` stage of that tag's `dev/Containerfile` with `scripts/ci/build-dev-container-stage.sh build-deps <tag>`. The script takes an explicit target from an allowlist (`build-deps`, `libvmaf-build`) and forwards the optional `github_token` BuildKit secret only to targets whose stage graph reaches the secret mount; `build-deps` does not, so the step sets no `GITHUB_TOKEN`. The script passes no `--build-arg`, no `--cache-from` / `--cache-to`, and pushes nothing.
3. Run `scripts/release/build-native-release-artifacts.sh` inside the built image with `docker run --pull never --network none`, as the runner's UID and GID, with the checkout mounted and `GITHUB_SHA` passed in. The script asserts the container marker, refuses to build unless `git rev-parse HEAD` equals `GITHUB_SHA` (the commit the provenance stamp records), builds with Meson, stages the libvmaf SONAME chain, the `vmaf` CLI, `models.tar.gz` and the optional u2netp mirror, stamps `container-build-provenance.txt` with `scripts/ci/check-container-build.sh --stamp`, and runs `scripts/release/verify-native-release-artifacts.sh`.
4. Hash and upload `artifacts/` on the host, unchanged, so the `hashes` output that feeds SLSA keeps the same format.

`build-artifacts` has no job-level concurrency group. The former cross-tag `release-artifacts-build` group only served the single self-hosted runner, and GitHub cancels an older pending job when a newer one joins its group. The workflow-level per-tag group stays. The job allows 60 minutes. Locally, uncached `build-deps` builds took 81 to 113 s and the release compile 148 s on four CPUs.

The Dev Container PR gate rehearses the release path on every container-affecting pull request. After its `libvmaf-build` build it builds `build-deps` through the same script (reusing the daemon's cached layers) and runs the release script with the same `docker run` flags. A pull request checkout reaches no tag, so the rehearsal creates a local lightweight tag `v<version>` on `HEAD`, where `<version>` comes from `.release-please-manifest.json`. `vmaf --version` then reports `v<version>-0-g<commit>`, as it does for a release, and the verifier checks it against the manifest version. Without the tag, Meson would fall back to `core/meson.build`'s project version, which equals the manifest only on a release commit.

Three build settings are explicit:

- **`-Denable_dnn=disabled`.** `build-deps` has no ONNX Runtime, so this changes nothing today. It is kept so a stage that does ship ONNX Runtime cannot turn the `auto` feature on and make `libvmaf.so` NEED `libonnxruntime.so.1`, which the release does not ship.
- **`CCACHE_DISABLE=1`.** `build-deps` installs ccache, and Meson wraps the compiler in it automatically. Run as a UID with no passwd entry in the image (any UID but 1000 and 2000), `HOME` is `/` and ccache fails every compile with `ccache: error: Permission denied`.
- **`gcc-ar`, `gcc-nm` and `gcc-ranlib` as `update-alternatives` slaves of `gcc`** in `build-deps`. The stage installs `gcc-13` but no default `gcc` package, so it had no unversioned `gcc-ar` and no `/usr/lib/bfd-plugins/liblto_plugin.so`. Meson then archived `libvmaf.a` with plain `ar`, which cannot index GCC LTO objects, and every LTO link of a unit test against it failed with undefined references. Later stages install Ubuntu's default GCC 15, which replaces those links, so `libvmaf-build` is unchanged.

`scripts/ci/check-container-build.sh` accepts exactly one identity, `image_title=vmaf-dev-mcp`, which `build-deps` writes and every later stage inherits. Its `--verify` mode rejects a symlinked stamp, as `verify-native-release-artifacts.sh` does. The retired runner title `vmaf-sycl-arc-runner` and the `vmafx-*` aliases are rejected; no image ever wrote them.

**Two-track exception.** `build-config.env`'s TWO TRACKS rule assigns published artifacts to the `RELEASE_*` track. The native bundle is built on the `DEV_*` track instead and needs glibc 2.43. It does not load on the release runtime image (`RELEASE_RUNTIME_CC`, distroless `cc-debian13`, glibc 2.41), on Debian 13 or on Ubuntu 24.04; each fails with `version 'GLIBC_2.43' not found`. `verify-native-artifacts` therefore runs on `ubuntu-26.04`. This exception holds for 1.0.0-rc.1 only as far as `docs/state.md` row `T-RELEASE-NATIVE-BUNDLE-RELEASE-TRACK-2026-09-27` allows: the native bundle moves to the Debian 13 release track before the final 1.0.0. Until then the operator adds the floor to each draft release's notes, a checklist step in the [release guide](../development/release.md#native-linux-release-layout) until ADR-1354 removed it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Hosted runner, `build-deps` stage (chosen)** | No workstation dependency; keeps ADR-1102 container provenance; digest-pinned base; no third-party downloads, so no GitHub token; at release time it contacts only Docker Hub (the digest-pinned `ubuntu:26.04` base and `docker/dockerfile` frontend) and the Ubuntu archive; the stage builds in about two minutes | Ubuntu archive packages resolve at image-build time, so rebuilding an old tag later can use newer compilers or Meson; glibc 2.43 floor; needed the `gcc-ar` alternatives fix before the default target set linked | Chosen (Q1): the smallest stage that carries the container marker and a complete C toolchain |
| Hosted runner, `libvmaf-build` stage (runner-up) | Same image the PR gate already smoke-tests; the stage developers use | ~29.5 GB; 28 to 35 minutes of uncached build per release; downloads Level Zero `.deb`s and `nv-codec-headers`, clones `vpl-gpu-rt`, SVT-AV1, VVenC and FFmpeg by tag, and fetches the ONNX Runtime tarball, none checked against a hash; a GitHub or vendor outage at publication time fails the release; brings ONNX Runtime and GCC 15 into the release compile | Q1: the release needs a C toolchain, not the GPU SDKs, and every extra download is an unpinned input |
| Debian 13 release-track builder (`RELEASE_BUILDER_BASE`) | Matches `build-config.env`'s two-track rule; glibc 2.41 bundle loads on Debian 13, the distroless release runtime and Ubuntu 24.04-class systems | New build image and marker identity for ADR-1102; needs its own proof before a release depends on it | Deferred (Q2) to before the final 1.0.0: `T-RELEASE-NATIVE-BUNDLE-RELEASE-TRACK-2026-09-27` |
| Bring the Arc runner up (keep ADR-1178) | No workflow change beyond a fail-fast admission probe; zero pull or build time | Every release depends on the workstation being on and the runner supervisor running; no runner is registered today; a missing runner costs a 24-hour queue | The maintainer chose a hosted option; a release should not depend on one machine |
| Hosted runner, no container | Smallest change; the bundle would keep the runner's glibc 2.39 floor | Breaks ADR-1102: a host-built binary would be published, and the provenance stamp cannot be produced honestly | Contradicts the container-only publishing policy |
| Pull `ghcr.io/vmafx/vmafx-dev-mcp` with a job-level `container:` | No in-job image build | Pull happens before any step can free disk; the image comes from a separately scheduled workflow; the release's toolchain would not come from the tag | Weaker provenance and the ADR-1178 disk concern |

## Consequences

- **Positive**:
  - Publishing a release no longer depends on the maintainer's workstation or a registered self-hosted runner.
  - Each release's toolchain comes from the tagged commit's own `dev/Containerfile`: a digest-pinned base plus Ubuntu archive packages. Building the stage pulls only digest-pinned images from Docker Hub (the `ubuntu:26.04` base and the `docker/dockerfile` frontend) and packages from the Ubuntu archive; nothing is restored from an external layer cache, no other registry image is used, no third-party download runs, and the release compile itself runs with networking disabled.
  - Every container-affecting pull request rehearses the release compile, stamp and verification with the release job's own script and flags.
  - The provenance gate accepts one identity and no symlinked stamp.
- **Negative**:
  - The native Linux bundle needs glibc 2.43 or newer (Ubuntu 26.04 or a distribution of the same generation) and does not load on Ubuntu 24.04, Debian 13 or the fork's distroless release runtime. The bundle has never been published, so no existing user is affected. Users on older distributions use the production containers or build from source. [`docs/development/release.md`](../development/release.md) states the floor and gives the release-notes section; adding it to each draft release is an operator checklist step, not an automated one.
  - The digest pin covers the base image only. Ubuntu archive packages (compilers, Meson) resolve when the stage is built, so two builds of the same tag weeks apart can use different distribution package versions.
  - The release compile uses GCC 13 from `build-deps`, while the developer image and `libvmaf-build` compile with GCC 15.
- **Neutral / follow-ups**:
  - `T-RELEASE-NATIVE-BUNDLE-RELEASE-TRACK-2026-09-27`: build the native bundle on the Debian 13 release track before the final 1.0.0 (an RC2/RC3 item).
  - `dev-container-publish.yml` stays a transparency publisher of `libvmaf-build` and is off the release path.
  - The `sycl-arc` label now carries only the SYCL parity lane of [ADR-1177](1177-sycl-arc-self-hosted-runner.md).
  - Release-candidate versions and the on-tag `vX.Y.Z[-rc.N]-0-g<hex>` form of `vmaf --version` are accepted by `verify-native-release-artifacts.sh` since #1570, so an `-rc.N` tag passes this job.

## References

- Supersedes [ADR-1178](1178-dev-container-image-publish.md).
- [ADR-1102](1102-phase4b9-container-only-publishing.md): container-only publishing policy.
- [ADR-0819](0819-dev-container-ci-gate.md): the PR gate that builds the `libvmaf-build` stage and now rehearses the release build.
- [ADR-1177](1177-sycl-arc-self-hosted-runner.md): the Arc runner, which keeps its SYCL parity role.
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md): first-release candidate dispositions.
- `build-config.env`, TWO TRACKS: `RELEASE_BUILDER_BASE="debian:13-slim@sha256:…"`, `RELEASE_RUNTIME_CC="gcr.io/distroless/cc-debian13:nonroot@sha256:…"`.
- Research digest: [docs/research/1346-hosted-slim-container-release-build.md](../research/1346-hosted-slim-container-release-build.md).
- GitHub Actions cache access and size limits: <https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching>.
- Source: popup, 2026-09-27. Question, verbatim: "Publishing v1.0.0-rc.1 runs supply-chain.yml, whose build-artifacts job targets the self-hosted [sycl-arc] runner (ADR-1178). No runner is registered (repo and org APIs return 0), so the job would queue 24h and cancel. The fix must be on master before #1213 merges. How should the native release artifacts be built?" Options, verbatim: "Bring the Arc runner up (Recommended)" — "Keep ADR-1178. I add a hosted admission probe so a missing runner fails fast instead of queueing 24h, and write down the exact runner-start steps. Before publishing the draft, you start the ADR-1177 runner supervisor on the workstation (repo-level registration; the org runner group disallows public repos)."; "Hosted runner, slim container" — "Supersede ADR-1178 with a new ADR: build-artifacts runs on a GitHub-hosted runner inside the small libvmaf-build container stage CI already builds (ADR-0819). Keeps container provenance without the 29.5 GB image; no workstation dependency. More change before RC1."; "Hosted runner, no container" — "Build directly on a hosted Ubuntu runner. Smallest change, but it breaks ADR-1102's rule that published artifacts are built inside the canonical container." Answer, verbatim: "Hosted runner, slim container".
- Q1, popup, 2026-09-27. Question, verbatim: "Correction to my earlier question: libvmaf-build is the ~30 GB stage, not a small one, and it downloads Level Zero, vpl-gpu-rt and ONNX Runtime at build time with no hash pins. dev/Containerfile has a genuinely small stage, build-deps: digest-pinned Ubuntu 26.04 + apt only (gcc-13, meson, ninja, nasm), same provenance marker, no third-party fetches, builds in minutes. Which stage should the hosted release build compile in?" Answer, verbatim: "build-deps (Recommended)".
- Q2, popup, 2026-09-27. Question, verbatim: "Container-built Linux binaries need glibc 2.43 (Ubuntu 26.04+), so they won't run on Ubuntu 24.04 or Debian 13, including the fork's own release runtime image (distroless Debian 13, glibc 2.41). build-config.env's two-track rule says published artifacts are built on the release track (Debian 13). ADR-1178 already broke this. What should RC1 do?" Answer, verbatim: "Accept for RC1, fix later (Recommended)" — option text, verbatim: "Ship RC1 with the glibc 2.43 floor, record the two-track exception in ADR-1346, state the floor in the release notes, and file a docs/state.md row to build the native bundle on the Debian 13 release track before the final 1.0.0 (an RC2/RC3 item)."
