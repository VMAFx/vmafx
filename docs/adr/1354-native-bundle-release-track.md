<!-- markdownlint-disable MD013 MD060 -->
# ADR-1354: Build the native Linux bundle on the Debian 13 release track

- **Status**: Accepted
- **Date**: 2026-09-28
- **Deciders**: lusoris
- **Tags**: ci, release, supply-chain, container, dev-container, adr-1102, fork-local

## Context

[ADR-1346](1346-hosted-slim-container-release-build.md) builds the native Linux release bundle (`libvmaf.so*` and the `vmaf` CLI) on a GitHub-hosted runner inside the `build-deps` stage of `dev/Containerfile`. That stage is Ubuntu 26.04 (`DEV_BASE`), so `libvmaf.so` binds `sqrtf@GLIBC_2.43`. The bundle therefore did not load on Debian 13, on Ubuntu 24.04, or on the fork's own release runtime image (`RELEASE_RUNTIME_CC`, distroless `cc-debian13`, glibc 2.41). `build-config.env`'s TWO TRACKS rule says published artifacts are built on the `RELEASE_*` track. The maintainer accepted the glibc 2.43 floor for 1.0.0-rc.1 only (ADR-1346 Q2) and asked for a `docs/state.md` row to move the bundle to the Debian 13 release track before the final 1.0.0: `T-RELEASE-NATIVE-BUNDLE-RELEASE-TRACK-2026-09-27`. Until then the operator had to paste the floor into every draft release's notes by hand.

[ADR-1102](1102-phase4b9-container-only-publishing.md) requires published artifacts to come from an image built from `dev/Containerfile` that carries the `/etc/vmafx-dev-container` marker, so the release-track compiler has to be a stage of that file rather than one of the `docker/` builders.

## Decision

We will compile the native bundle in a new `release-build` stage of `dev/Containerfile`. It is a separate root, `FROM ${RELEASE_BUILDER_BASE}` (digest-pinned `debian:13-slim`), with Debian's `gcc`, `g++`, `meson`, `ninja-build`, `nasm`, `pkg-config`, `git`, `xxd` and `patchelf` pinned to Debian 13's `0.18.0-1.4`. It writes the same marker bytes as `build-deps`. `scripts/ci/build-dev-container-stage.sh` allowlists `release-build` in place of `build-deps`, and `supply-chain.yml` `build-artifacts` and the Dev Container PR-gate rehearsal both build it; the runner, script, `docker run` flags and rehearsal of ADR-1346 are otherwise unchanged. The release compile adds `-Denable_tests=false`, because Debian 13's GCC 14.2 crashed at random while link-time optimising the unit tests. `verify-native-artifacts` moves from `ubuntu-26.04` to `ubuntu-24.04`, the oldest GitHub-hosted image that loads the bundle, and also starts the downloaded CLI in `RELEASE_RUNTIME_CC`. The operator's release-notes step is removed. This amends ADR-1346.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the Ubuntu 26.04 `build-deps` build and document the glibc 2.43 floor | No change; one compiler generation for dev and release | The bundle does not load on Debian 13, Ubuntu 24.04 (the most widely deployed LTS and today's `ubuntu-latest`) or the fork's own release runtime; breaks the TWO TRACKS rule; every release needs a hand-written release-notes section | ADR-1346 Q2 accepted it for RC1 only, with the release-track build due before the final 1.0.0 |
| **Build on the Debian 13 release track in a `release-build` stage of `dev/Containerfile` (chosen)** | Same base as every other release artifact; measured floor `GLIBC_2.38` + `GLIBCXX_3.4.30`, so it loads on Ubuntu 24.04, Debian 13, distroless `cc-debian13` and newer; keeps the ADR-1102 marker and the hosted-runner flow; the stage builds in under a minute and needs no GitHub token | A second marker write to keep identical (unit-tested); a second compiler generation (GCC 14.2 for release, GCC 13/15 in the dev stages); GCC 14.2 needs the unit tests left out of the release compile | Chosen |
| Static linking (`-static`, or `-static-libstdc++ -static-libgcc` on the dev-track build) | Could lower or remove the libstdc++ requirement | `libvmaf.so` is a shared library and its SONAME chain is the published ABI; a fully static glibc CLI breaks `dlopen` and NSS and is unsupported by glibc; static libstdc++ still leaves the glibc 2.43 floor of the build host, so it fixes nothing the release track does not; changes the asset set, SBOMs and verifier | Solves the wrong half of the problem and changes the release layout |
| Debian 13 stage but keep compiling the unit tests | Release compile covers the full default target set | GCC 14.2 (Debian `14.2.0-19`) hit internal compiler errors in `lto1` (`Segmentation fault`, then `corrupted size vs. prev_size`) while LTO-linking test executables in 2 of 3 full builds; a release would fail by chance | The release ships no tests; other CI jobs build and run them |
| Build in a `docker/` builder (for example `docker/Dockerfile.production`'s `builder`) | Already Debian 13 and already built for releases | Not `dev/Containerfile`, carries no ADR-1102 marker, and bakes the source into image layers | Violates ADR-1102 |

## Consequences

- **Positive**:
  - The native bundle loads on glibc 2.38 and newer with the libstdc++ of GCC 12 or newer: Ubuntu 24.04, Debian 13, the distroless release runtime and newer distributions. Each release checks Ubuntu 24.04 (full verifier on the runner), the release runtime image (CLI start) and Debian 13 (verifier inside the build stage).
  - The published bundle, the CPU container image and the node image now share one compiler and C library generation.
  - No manual release-notes step: the release guide states the requirement once.
  - The release compile takes well under a minute instead of several.
- **Negative**:
  - Debian archive packages other than `patchelf` resolve when the stage is built, so a later rebuild of an old tag may use a compiler from a newer Debian point release. The base digest pin is unchanged.
  - The release compiler (GCC 14.2) differs from the developer image (GCC 15) and `build-deps` (GCC 13). Numerical parity between them is covered by the existing CI gates, not by the release job.
  - Ubuntu 22.04 and Debian 12 remain unsupported (`GLIBC_2.38` missing).
- **Neutral / follow-ups**:
  - `patchelf` serves the RUNPATH fix (`T-RELEASE-NATIVE-RUNPATH-BUILD-TREE-2026-09-27`), which stages the CLI with `patchelf --set-rpath '$ORIGIN'`. It moves with the compile from `build-deps` (Ubuntu `0.18.0-1.4build1`) to `release-build` (Debian `0.18.0-1.4`); `build-deps` no longer installs it. The release-runtime check runs the CLI with no `LD_LIBRARY_PATH`, so it proves that RUNPATH on the distroless image too.
  - A symbol that needs glibc newer than 2.39 would first fail at release time in `verify-native-artifacts`; a static symbol-floor check at PR time is possible future work.
  - `build-deps` keeps its `gcc-ar` alternatives fix; no CI job builds it on its own any more.

## Supply-chain impact

- **New dependencies**: build-time only, from the Debian 13 archive in `release-build`: `gcc`/`g++` 14.2.0-19, `meson` 1.7.0-1, `ninja-build` 1.12.1-1, `nasm` 2.16.03-1, `xxd` 9.1.1230-2, `git`, `pkg-config`, `patchelf` 0.18.0-1.4 (pinned), as installed on 2026-09-28.
- **Removed dependencies**: the Ubuntu 26.04 `build-deps` packages leave the release path.
- **Build-time fetches**: the digest-pinned `RELEASE_BUILDER_BASE` from Docker Hub and packages from `deb.debian.org`; nothing from third parties, no GitHub token.

## References

- Amends [ADR-1346](1346-hosted-slim-container-release-build.md); keeps [ADR-1102](1102-phase4b9-container-only-publishing.md) and [ADR-0819](0819-dev-container-ci-gate.md).
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md): first-release candidate dispositions.
- Research digest: [docs/research/1354-native-bundle-release-track.md](../research/1354-native-bundle-release-track.md).
- `build-config.env`, TWO TRACKS: `RELEASE_BUILDER_BASE`, `RELEASE_RUNTIME_CC`.
- Source: ADR-1346 Q2 (popup, 2026-09-27), maintainer answer "Accept for RC1, fix later (Recommended)", option text verbatim: "Ship RC1 with the glibc 2.43 floor, record the two-track exception in ADR-1346, state the floor in the release notes, and file a docs/state.md row to build the native bundle on the Debian 13 release track before the final 1.0.0 (an RC2/RC3 item)." The `release-build` stage shape, `-Denable_tests=false` and the `ubuntu-24.04` verifier are implementation choices backed by the measurements in the research digest.
