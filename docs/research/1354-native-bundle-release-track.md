<!-- markdownlint-disable MD013 MD060 -->
# Research-1354: Building the native Linux bundle on the Debian 13 release track

- **Status**: Active
- **Workstream**: [ADR-1354](../adr/1354-native-bundle-release-track.md), amends [ADR-1346](../adr/1346-hosted-slim-container-release-build.md)
- **Last updated**: 2026-09-28

## Question

Can the native release bundle be compiled on `RELEASE_BUILDER_BASE` (Debian 13) in a stage of `dev/Containerfile` that carries the ADR-1102 marker, and does the result load on Debian 13, Ubuntu 24.04 and the release runtime image `RELEASE_RUNTIME_CC`? Which hosted image is the oldest one the release can verify on? Does the release script need to change?

## Sources

- `dev/Containerfile` `release-build` stage, built with `bash scripts/ci/build-dev-container-stage.sh release-build <tag>` on the maintainer workstation (Docker 29.8.0, buildx 0.37.1, 12 CPUs, 12.5 GB for the Docker VM), 2026-09-28.
- `scripts/release/build-native-release-artifacts.sh 1.0.0-rc.1`, run as in `supply-chain.yml`: `docker run --pull never --network none --user 1001:1001` (no passwd entry) with `GITHUB_SHA` set to the checkout's `HEAD`, on a throwaway Linux clone of the branch tagged `v1.0.0-rc.1`. Four-CPU runs used `--cpuset-cpus=0-3` to match a hosted runner.
- `objdump -T`, `readelf -d` and `readelf -V` from Debian 13 binutils 2.44 inside the stage.
- `ldd --version` and `vmaf --version` in `debian:13-slim`, `ubuntu:24.04`, `ubuntu:26.04`, `ubuntu:22.04` and `gcr.io/distroless/cc-debian13:nonroot@sha256:54df941e…` (the `RELEASE_RUNTIME_CC` pin).
- The RUNPATH fix for `T-RELEASE-NATIVE-RUNPATH-BUILD-TREE-2026-09-27` (commit `d0959fbe0`), first applied on top for one combined run, then used as the base of this change.

## Findings

### The stage

`release-build` installs, from the Debian 13.7 archive: GCC 14.2.0-19, binutils 2.44-3, Meson 1.7.0-1 (the project needs ≥ 1.4.0), Ninja 1.12.1-1, NASM 2.16.03-1, `xxd` 9.1.1230-2, Git, pkg-config and `patchelf` 0.18.0-1.4 (pinned). Debian's default `gcc` package provides `gcc-ar` and `/usr/lib/bfd-plugins/liblto_plugin.so`, so the `update-alternatives` fix `build-deps` needs is not needed here. `xxd` is required: `core/src/meson.build` embeds the built-in models only when it finds `xxd`. An uncached build of the stage (base pull plus apt) took 51 s; with the base cached it took 42 s. `docker build --check` reports no warnings for `release-build` or `libvmaf-build`.

### The GCC 14.2 LTO crash

With the unit tests enabled (the ADR-1346 flags), 2 of 3 full release builds failed while LTO-linking test executables, with different errors each time:

| Run | CPUs | Result |
|---|---|---|
| 1 | 12 | `test_integer_adm_simd_noise`: `internal compiler error: Segmentation fault` in `float_ms_ssim.c` `extract` (GIMPLE pass `pre`); relinking the same target alone succeeded |
| 2 | 4 | passed in 354 s (1570 targets) |
| 3 | 12 | `test_float_vif_coverage`: `corrupted size vs. prev_size`, then `internal compiler error: Aborted` in `ssimulacra2_avx2.c` (pass `lversion`, `gori_map::gori_map()` in the backtrace) |

The crash is inside the compiler (heap corruption detected by glibc in `lto1`) and does not reproduce on a retry of the same link. With `-Denable_tests=false` the build has 234 targets and 8 LTO links; 22 consecutive builds passed (20 on 12 CPUs in 19 to 39 s, two full release-script runs on 4 CPUs in 24 and 26 s). `docker/Dockerfile.production`, `Dockerfile.node` and `Dockerfile.controller` already configure `-Denable_tests=false` on the same base.

### Symbol floor of the bundle

| File | NEEDED | Newest versions |
|---|---|---|
| `libvmaf.so.3.0.0` | `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6` | `GLIBC_2.38` (`__isoc23_strtol`), `GLIBCXX_3.4.30` (`std::__glibcxx_assert_fail`), `CXXABI_1.3.9`, `GCC_3.3.1` |
| `vmaf` | `libvmaf.so.3`, `libstdc++.so.6`, `libgcc_s.so.1`, `libc.so.6` | `GLIBC_2.38` (`__isoc23_strtol`, `__isoc23_strtoul`, `__isoc23_sscanf`), `GLIBCXX_3.4.20` |

No symbol needs a glibc newer than 2.38. The `__isoc23_*` entry points come from compiling in C23 mode against glibc ≥ 2.38 headers. The ADR-1346 bundle bound `sqrtf@GLIBC_2.43`.

### Where the bundle loads

`vmaf --version` with the bundle directory on `LD_LIBRARY_PATH` (this branch alone, whose CLI still carries the build-tree RUNPATH):

| Image | glibc | Result |
|---|---|---|
| `debian:13-slim` | 2.41 | `v1.0.0-rc.1-0-g<commit>` |
| `ubuntu:24.04` | 2.39 | same |
| `ubuntu:26.04` | 2.43 | same |
| `gcr.io/distroless/cc-debian13:nonroot` (`RELEASE_RUNTIME_CC`) | 2.41 | same; a 48-frame 576x324 score with the default model also completed (pooled VMAF 95.938229) |
| `ubuntu:22.04` | 2.35 | `version 'GLIBC_2.38' not found` for `vmaf` and `libvmaf.so.3` |

The full clean-environment verifier (`verify-native-release-artifacts.sh`) passed on `ubuntu:24.04` with `binutils` installed, which is what the `ubuntu-24.04` runner provides. `ubuntu-24.04` is therefore the oldest GitHub-hosted image that can verify the bundle.

Rebased on the RUNPATH fix (the `patchelf` pin moved from `build-deps` to `release-build`), the release script staged `vmaf` with `RUNPATH [$ORIGIN]` and its verifier passed inside the stage. Without `LD_LIBRARY_PATH`, the CLI printed its version on `debian:13-slim`, `ubuntu:24.04`, `ubuntu:26.04` and the distroless runtime (run as its `nonroot` user with `--network none`), and `ldd` resolved `libvmaf.so.3` next to it. The fix's verifier also passed on `ubuntu:24.04`.

## Alternatives explored

- **Keep the Ubuntu 26.04 build and document the floor**: the status quo of ADR-1346; see the ADR table.
- **Static linking**: does not remove the glibc floor of the build host and changes the published ABI surface.
- **Keep compiling the unit tests**: fails at random with GCC 14.2 (table above).
- **Debian's `gcc-13` (13.3.0-16) instead of the default GCC 14.2**: available in the archive, but a non-default compiler needs its own `gcc-ar` alternatives, as in `build-deps`, and the crash only affects the test links the release does not need.
- **A `docker/` builder**: carries no ADR-1102 marker.

## Open questions

- Hosted-runner timings for the stage and the compile come from the first rehearsal run on a pull request.
- Whether GCC 14.3 or a later Debian point release fixes the `lto1` crash; if the release compile ever needs the tests again, re-measure first.

## Related

- ADRs: [ADR-1354](../adr/1354-native-bundle-release-track.md), [ADR-1346](../adr/1346-hosted-slim-container-release-build.md), [ADR-1102](../adr/1102-phase4b9-container-only-publishing.md), [ADR-0819](../adr/0819-dev-container-ci-gate.md)
- Prior digest: [Research-1346](1346-hosted-slim-container-release-build.md)
