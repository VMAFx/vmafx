<!-- markdownlint-disable MD013 MD060 -->
# Research-2128: oneAPI release image — Arc B580 crash and the Debian 13 rebuild — 2026-09-29

- **Status**: Active
- **Workstream**: RC3 release image; decision in [ADR-1368](../adr/1368-oneapi-release-image-debian13.md); closes `T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29`
- **Last updated**: 2026-09-29

## Question

The published oneAPI image (`final-oneapi2025`) exits 139 on an Arc B580 right after "SYCL: using device: Intel(R) Graphics [0xe20b]", while a UHD 770 scores. Is the old Intel GPU compute runtime in Intel's `oneapi-runtime:2025.3.1` image the cause, does the runtime alone fix it, and what does the image look like when it follows the design `build-config.env` already records (oneAPI 2026.1 from Intel's apt repository on Debian 13)?

## Sources

- `build-config.env` oneAPI block (soname and glibc measurements behind the 2026.1 target design); the base-image gate's `ONEAPI_BUILDER` / `ONEAPI_RUNTIME` exemption comment.
- [ADR-0541](../adr/0541-dev-container-sycl-hip-runtime-fix.md) (NEO 25.18 too old; NEO from GitHub releases), [ADR-1145](../adr/1145-neo-stack-derived-from-release.md), [ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md).
- Intel's oneAPI apt repository index, `https://apt.repos.intel.com/oneapi/dists/all/main/binary-{amd64,all}/Packages.gz` (Release dated 2026-09-08).
- GitHub release metadata of `oneapi-src/level-zero` v1.34.0 (asset `digest` fields) and `intel/compute-runtime` 26.35.39758.10.

## Setup

Windows 11 host, Docker Desktop 29.8.1 with the WSL 2 backend, Arc B580 (`level_zero:0`, `0xe20b`) and UHD 770 (`level_zero:1`, `0x4680`) passed through `/dev/dxg` with `/usr/lib/wsl` mounted and `/usr/lib/wsl/lib` appended to the image's `LD_LIBRARY_PATH`. Build contexts came from a WSL clone at `master` `2d9d5b069`. Every GPU run held `flock /f/gpu.lock` on the shared fixtures volume. Throwaway probe Dockerfiles were not committed.

## Findings

### 1. `master`'s 2025 builder no longer links the unit tests

`docker build --target final-oneapi2025` at `2d9d5b069` fails at `[952/1781] Linking target test/test_iqa_helpers`: `libsycl-devicelib-host.a(fallback-imf-fp32-host.o): undefined reference to symbol 'log10f@@GLIBC_2.2.5'` (and `fesetround`), `libm.so.6: error adding symbols: DSO missing from command line`. `-lm` precedes the device library that icpx 2025.3 appends, and `--as-needed` has dropped it by then. The probe images below were therefore built with `-Denable_tests=false` in the oneAPI builder only. The 2026.1.1 compiler links every test on the same tree.

### 2. The compute runtime alone decides the crash

Same 2025-built binary in every row (probe of `final-oneapi2025` at `2d9d5b069`, AOT build); default model, Netflix pair, 2 frames. The variant images replaced packages with the `dev/scripts/fetch-intel-neo.py` runtime set at `INTEL_NEO_VERSION` and/or `libze1` 1.34.0.

| Variant | `libze-intel-gpu1` (NEO) | IGC | Level Zero loader | Arc B580 | UHD 770 |
|---|---|---|---|---|---|
| image as published | 25.18.33578.15 | 2.11.12 | 1.21.9 | exit 139 | exit 0 |
| loader only | 25.18.33578.15 | 2.11.12 | 1.34.0 | exit 139 | exit 0 |
| NEO only | 26.35.39758.10 | 2.41.5 | 1.21.9 | exit 0 | exit 0 |
| NEO + loader (dev container set) | 26.35.39758.10 | 2.41.5 | 1.34.0 | exit 0 | exit 0 |

Every exit-0 row printed identical per-feature means on both GPUs (for example `integer_adm2` 0.954507, `cambi` 0.307775). The Level Zero loader version is irrelevant; the NEO GPU driver and its IGC are the fix. That matches ADR-0541's finding for the development container.

### 3. What Intel's apt repository provides

- The 2026.1 compiler is `intel-oneapi-compiler-dpcpp-cpp-2026.1` at 2026.1.0-235 and 2026.1.1-325; the runtime meta `intel-oneapi-runtime-dpcpp-cpp` carries 2026.0.0-947, 2026.1.0-235 and 2026.1.1-325. Every package of a release shares its build number, but the metas depend on their sub-packages with `>=`, so apt would take the newest build in the repository for each sub-package unless pinned. An apt preference `Package: intel-oneapi-*` / `Pin: version 2026.1.1-325` holds them; UMF (`intel-oneapi-umf-1.1` 1.1.0-340) has its own version line and its own pin.
- The runtime closure on Debian 13 is 16 packages, 1.1 GB under `/opt/intel/oneapi`, with `libsycl.so.9`, `libur_loader`, the three Unified Runtime adapters, `libsvml`, `libirc` and the OpenCL CPU device in `redist/lib`, which its `ld.so.conf.d` entry registers. `libumf.so.1` lands in `/opt/intel/oneapi/umf/1.1/lib` with a `latest` symlink and is not on the loader path, so the image keeps it on `LD_LIBRARY_PATH`. No package ships a Level Zero loader.
- The key file at `intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB` (sha256 `db932ba0…c48c` on 2026-09-29) holds five keys, four expired. The valid one, `E9BF0AFC46D6E8B7DA5882F1BAC6F0C353D04109`, expires 2027-08-21. Exporting only that key into the keyring `apt` uses for the repository verifies the repository. Importing the file with `gpg` exits 2 when `gpg-agent` is absent (a Recommends of `gpg`), although the keys are imported; the installer tolerates that exit and decides on the exported fingerprint.
- The Level Zero release publishes `.sig` files but no checksum list. GitHub's release API reports a `sha256:` `digest` per asset, which the fetcher checks. The `+u24.04` debs need glibc 2.39 and install on Debian 13.

### 4. The rebuilt image

`final-oneapi2026` from this change, built on the same host with `VMAF_BUILD_JOBS=3`:

| | `final-oneapi2025` at `2d9d5b069` (probe) | `final-oneapi2026` |
|---|---|---|
| Base | `intel/oneapi-runtime:2025.3.1` (Ubuntu 24.04) | `debian:13-slim` |
| SYCL runtime | 2025.3.1, `libsycl.so.8` | 2026.1.1-325, `libsycl.so.9` |
| NEO / IGC | 25.18.33578.15 / 2.11.12 | 26.35.39758.10 / 2.41.5 |
| Level Zero loader | 1.21.9 | 1.34.0 |
| Size, unpacked | 5.86 GB | 2.39 GB |
| Size, `docker save` + gzip -6 | 1.42 GB | 0.61 GB |

The builder's `sycl_aot_image_check` reported "30 spir64_gen fat binaries; 13 IP versions for 19 targets; 0 partial by declaration" (ADR-1360). The final stage has no curl, gpg or python3, runs as 65532:65532, resolves every adapter and `vmaf` dependency, and prints `1.0.0-rc.2` for `vmaf --version`.

### 5. Scores against the CPU backend of the same image

Netflix pair `src01_hrc00/hrc01` 576x324, all 48 frames; BBB 3840x2160, first 50 frames. Tolerances are the parity gate's (`scripts/ci/cross_backend_parity_gate.py` `FEATURE_TOLERANCE`, `metric_delta`, `area_tolerance_factor`; the VMAF score at places=4), compared per frame and per metric. Each JSON's `feature_backends` confirms the SYCL twin ran (`ssimulacra2_sycl`, `psnr_hvs_sycl`, the model's SYCL extractors).

| Pair | Workload | Device | CPU pooled | SYCL pooled | Worst per-frame delta (tolerance) | Result |
|---|---|---|---|---|---|---|
| Netflix, 48 frames | default model (`vmaf`) | Arc B580 | 82.816059 | 82.816058 | `integer_motion2` 1.2e-5 (5e-5) | pass |
| Netflix, 48 frames | default model (`vmaf`) | UHD 770 | 82.816059 | 82.816058 | `integer_motion2` 1.2e-5 (5e-5) | pass |
| Netflix, 48 frames | `--feature psnr_hvs` | Arc B580 | 31.330446 | 31.330390 | `psnr_hvs_y` 8.3e-5 (5e-4) | pass |
| Netflix, 48 frames | `--feature psnr_hvs` | UHD 770 | 31.330446 | 31.330390 | `psnr_hvs_y` 8.3e-5 (5e-4) | pass |
| Netflix, 48 frames | `--feature ssimulacra2` | Arc B580 | 24.614428 | 24.614428 | 0 on every frame (5e-3) | pass |
| Netflix, 48 frames | `--feature ssimulacra2` | UHD 770 | 24.614428 | 24.614428 | 0 on every frame (5e-3) | pass |
| BBB 4K, 50 frames | default model (`vmaf`) | Arc B580 | 79.182228 | 79.182227 | `integer_motion2` 5e-6 (5e-5) | pass |
| BBB 4K, 50 frames | default model (`vmaf`) | UHD 770 | 79.182228 | 79.182227 | `integer_motion2` 5e-6 (5e-5) | pass |
| BBB 4K, 50 frames | `--feature psnr_hvs` | Arc B580 | 44.258029 | 44.257588 | `psnr_hvs_y` 8.43e-4 (3.34e-3, area-scaled) | pass |
| BBB 4K, 50 frames | `--feature psnr_hvs` | UHD 770 | 44.258029 | 44.257588 | `psnr_hvs_y` 8.43e-4 (3.34e-3, area-scaled) | pass |
| BBB 4K, 50 frames | `--feature ssimulacra2` | Arc B580 | 65.881933 | 65.881933 | 0 on every frame (5e-3) | pass |
| BBB 4K, 50 frames | `--feature ssimulacra2` | UHD 770 | 65.881933 | 65.881933 | 0 on every frame (5e-3) | pass |

The two GPUs agree with each other bit for bit at the printed precision in every row.

## Alternatives explored

- **Overlay NEO 26.35 on the 2025 runtime image.** It fixes the B580 (row "NEO only" above), but the 2025.3.2 builder cannot link `master`'s tests (finding 1), and it keeps Ubuntu 24.04, a 5.9 GB image and a SYCL runtime one major behind the development container.
- **Intel's 2026 images.** Rejected by the measurements already in `build-config.env`: the runtime image stops at 2026.0 (older than the 2026.1 compiler) and the Ubuntu 26.04 glibc is newer than Debian 13's.
- **Debian 13's own `libze1`.** Not `LEVEL_ZERO_VERSION`; the loader version is otherwise irrelevant to the crash (finding 2), so the single pinned version wins.

## Open questions

- A native Linux host was not available for this run. WSL 2 reaches the GPUs through `/dev/dxg` and the host driver's user-space half in `/usr/lib/wsl/lib`, a different kernel interface from a Linux `i915`/`xe` render node; the maintainer's Linux box (RTX 4090 + Arc A380) has the copy-paste check in `docs/state.md`.
- The Arc A380 was not tested with the new image here.

## Related

- [ADR-1368](../adr/1368-oneapi-release-image-debian13.md), `docs/state.md` row `T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29`, [docker-production.md](../development/docker-production.md#oneapi-20261-sycl-intel-arc).
