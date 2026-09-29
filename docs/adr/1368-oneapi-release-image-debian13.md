<!-- markdownlint-disable MD013 MD060 -->
# ADR-1368: Build and run the oneAPI release image on Debian 13 with pinned Intel packages

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: release, docker, container, sycl, gpu, intel, supply-chain, fork-local

## Context

The published oneAPI image (`docker/Dockerfile.production-gpu`, stage `final-oneapi2025`, tag `-oneapi2025`) crashed every `vmaf --backend sycl` run on an Arc B580 (Xe2, `0xe20b`) with a segmentation fault right after device selection, while a UHD 770 scored normally (`T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29`). The image compiled in `intel/oneapi-basekit:2025.3.2` and ran on `intel/oneapi-runtime:2025.3.1`, whose Intel GPU compute runtime (NEO) is 25.18.33578.15 with IGC 2.11. [ADR-0541](0541-dev-container-sycl-hip-runtime-fix.md) had already found that runtime too old and moved the development container to GitHub releases of compute-runtime, now pinned as `INTEL_NEO_VERSION` (26.35.39758.10).

A probe confirmed the cause before any design work (see the [digest](../research/2128-oneapi-release-image-runtime.md)). The unchanged 2025-built binary, with only the GPU runtime packages swapped for NEO 26.35 from `dev/scripts/fetch-intel-neo.py`, scored the Netflix pair on the B580 exactly as on the UHD 770. Swapping only the Level Zero loader (1.21.9 to 1.34.0) left the crash in place.

`build-config.env` already described the intended design: oneAPI compiler and runtime from Intel's apt repository on Debian 13 at `ONEAPI_VERSION` 2026.1, because the SYCL soname changed from `libsycl.so.8` (2025) to `.so.9` (2026), Intel's runtime image stops at 2026.0 while its toolkit image carries 2026.1, and both Intel images are Ubuntu builds whose 26.04 glibc (2.43) is newer than Debian 13's (2.41). The base-image gate carried `ONEAPI_BUILDER` and `ONEAPI_RUNTIME` as the last distro exemptions for exactly that follow-up. The production image never made the move, and on current `master` the 2025.3.2 builder no longer links the unit tests: `libsycl-devicelib-host.a` needs `log10f` and `fesetround` from libm after `--as-needed` has dropped it. The 2026.1 compiler links them.

The image's stage name and tag suffix both say 2025. Published tags are an interface (HISS-14), and scripts compose `$tag-oneapi2025`.

## Decision

We will build and run the oneAPI release image on `RELEASE_BUILDER_BASE` (`debian:13-slim`, digest-pinned) and install every Intel component from pinned inputs named in `build-config.env`:

- `scripts/ci/install-intel-oneapi.sh --mode=builder|runtime` installs Intel's oneAPI compiler (`ONEAPI_APT_PACKAGE`) in the builder and the SYCL runtime plus UMF (`ONEAPI_RUNTIME_APT_PACKAGES`) in the final stage, both at the exact Intel build `ONEAPI_APT_VERSION` (2026.1.1-325), UMF at `ONEAPI_UMF_APT_VERSION`. An apt preference holds every sub-package of the release at that build. apt trusts only the repository key whose fingerprint is `INTEL_ONEAPI_APT_SIGNER_FINGERPRINT`.
- `scripts/ci/install-intel-ocloc.sh` gains `--components build|runtime`. `build` adds the Level Zero loader and headers at `LEVEL_ZERO_VERSION` to ocloc; `runtime` installs the whole NEO set the development container installs (Level Zero GPU driver, OpenCL ICD, gmmlib, IGC, ocloc) at `INTEL_NEO_VERSION`, plus the loader. `dev/scripts/fetch-intel-neo.py` fetches the loader and verifies it against the SHA-256 digest GitHub records for each release asset, since that release publishes no checksum file.
- `ONEAPI_BUILDER` and `ONEAPI_RUNTIME` equal `RELEASE_BUILDER_BASE`, and the gate enforces it as it enforces the CUDA bases against `DEV_BASE`. The distro exemption list becomes empty.
- The stage is `final-oneapi2026` and the image is tagged `<tag>-oneapi2026`. `final-oneapi2025` stays as an alias stage and every release also tags the same digest `<tag>-oneapi2025`, until a release that announces a breaking change retires them.
- The node image's unpublished `node-sycl` target takes its SYCL runtime from the same installer instead of Intel's 2025 image.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep Intel's 2025 images and overlay NEO 26.35 on the runtime image | Smallest diff; the probe proves it stops the crash | The 2025.3.2 builder no longer links `master`'s tests; keeps the Ubuntu 24.04 exemption, a 5.9 GB runtime image, and a SYCL runtime a major version behind the development container and CI (2026.1) | Fixes the symptom and leaves every documented reason for the move in place |
| Intel's 2026 images (`intel/oneapi:2026.1` builder, `intel/oneapi-runtime:2026.0` runtime) | No apt handling | Runtime older than the compiler; Ubuntu 26.04 glibc 2.43 against the Debian 13 release track; the runtime image still ships whatever NEO Intel chose | Measured in `build-config.env`; the pairing is unsound |
| **Debian 13 with Intel's apt packages and the pinned NEO and loader (chosen)** | One glibc and one base across CPU and oneAPI images; compiler and runtime from one exact build; the same GPU runtime as the development container; the image shrinks | The image builds depend on Intel's apt repository and two GitHub releases | Chosen |
| Level Zero loader from Debian 13 (`libze1`) | apt-native | Debian's version, not `LEVEL_ZERO_VERSION`; `build-config.env` exists because four loader versions were in the tree at once | Keeps one loader version |
| A new NEO installer for the image | No change to the CI installer | A second implementation of what `install-intel-ocloc.sh` and `fetch-intel-neo.py` already do (HISS-19) | Extended the existing installer instead |
| Switch `dev/Containerfile` to the extended installer in the same change | One call site pattern everywhere | Its NEO step is bound to the BuildKit-secret contract (ADR-1271) and its loader download to `check-workflow-versions.py`; both would need rework and a full development-image rebuild | Same package set through the same fetcher already; left for a follow-up |
| Rename the tag and stage to `2026` only | Accurate names | Breaks every caller of `-oneapi2025` and `--target final-oneapi2025` (HISS-14) | Aliases keep them working |
| Keep only the `2025` names | No new names | The names would describe a release the image no longer contains | Misleading |
| A version-free `-oneapi` tag | Survives future bumps | A third name for one image; the other GPU tags carry the SDK major (`-cuda13`, `-rocm10`) | Consistency with the tag matrix |

## Consequences

- **Positive**: the image scores on the Arc B580. The oneAPI image now sits on the same Debian 13 glibc as the CPU image and the native release bundle, and runs the same compute runtime as the development container. The last base-image distro exemption is gone.
- **Positive**: every Intel input is pinned by exact version and verified: apt signatures under a pinned key, published SHA-256 sums for compute-runtime and IGC, GitHub asset digests for the Level Zero loader. The build fails on an unresolved adapter or `vmaf` library dependency.
- **Negative**: a rotated Intel apt key or a removed package build fails the image build until `build-config.env` is updated; this is deliberate. Moving `ONEAPI_VERSION` alone fails the installer until `ONEAPI_APT_VERSION` names a build of the new release.
- **Negative**: the build now calls the GitHub API for the compute-runtime, IGC and Level Zero releases in both stages. The publish workflow passes `GITHUB_TOKEN` as the optional BuildKit secret `github_token` to avoid the anonymous rate limit.
- **Neutral / follow-ups**: a recovery dispatch that builds a pre-rc.3 tag with this recipe (ADR-1347) cannot build the oneAPI image, because the old tag lacks the installers and knobs; this was already true for `install-intel-ocloc.sh` since ADR-1360. `dev/Containerfile` keeps its own NEO and loader steps. The CI SYCL legs still add Intel's apt repository inline and build the loader from source; moving them to `install-intel-oneapi.sh` and `--components build` is a follow-up. `node-sycl` builds libvmaf without SYCL, so the runtime it copies is unused; unchanged here.

## Supply-chain impact

- **New dependencies**: none new in kind. The image now installs `intel-oneapi-runtime-dpcpp-cpp` 2026.1.1-325 and `intel-oneapi-umf` 1.1.0-340 (runtime), `intel-oneapi-compiler-dpcpp-cpp-2026.1` 2026.1.1-325 (build), compute-runtime 26.35.39758.10 with IGC 2.41.5 and gmmlib 22.10.0 (runtime), and Level Zero loader 1.34.0 (runtime and build).
- **Removed dependencies**: `intel/oneapi-basekit:2025.3.2` and `intel/oneapi-runtime:2025.3.1` images, and with them compute-runtime 25.18 and the rest of the runtime image's Ubuntu 24.04 userland.
- **Build-time fetches**: Intel's apt repository (signed; key pinned by fingerprint), GitHub release assets of `intel/compute-runtime`, `intel/intel-graphics-compiler` and `oneapi-src/level-zero` (SHA-256 verified).
- **Sigstore-signable**: the image is signed and attested as before.
- **CVE surface delta**: narrower: a Debian 13 slim userland replaces Ubuntu 24.04 with Intel's development runtime; curl, gpg and python3 leave the final image with the installers.

## References

- Research digest: [Research-2128](../research/2128-oneapi-release-image-runtime.md).
- [ADR-0541](0541-dev-container-sycl-hip-runtime-fix.md) (NEO from GitHub releases), [ADR-1145](1145-neo-stack-derived-from-release.md) (NEO set derived from one release), [ADR-1231](1231-base-image-single-source.md) (base-image single source), [ADR-1306](1306-drop-nvidia-cuda-base.md) (the CUDA precedent: distro base plus exact vendor packages), [ADR-1347](1347-image-recovery-from-default-branch.md) (recovery dispatch), [ADR-1360](1360-sycl-aot-compile-time-device-codegen.md) (AOT images and their build-time check).
- Source: lead brief of 2026-09-29: "Close `T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29`", "move the oneAPI builder + final stage to the documented design", "extend that one installer rather than writing a second (HISS-19)", "do not drop or rename a published tag; if the stage/tag name carries \"2025\" and the content moves to 2026.1, keep the old tag working (alias) and add the accurate one".
