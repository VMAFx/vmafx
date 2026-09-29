<!-- markdownlint-disable MD013 MD060 -->
# Research-2125: First native Windows run of the SYCL build on Arc B580 and UHD 770 — 2026-09-29

- **Status**: Active
- **Workstream**: RC3 fix-and-revalidate ([ADR-1341](../adr/1341-rc-correctness-benchmark-retrain-sequence.md)); decision in [ADR-1364](../adr/1364-windows-sycl-msvc-device-link.md)
- **Last updated**: 2026-09-29

## Question

The `Windows MSVC+SYCL` CI leg ([ADR-0121](../adr/0121-windows-gpu-build-only-legs.md)) configures and builds on a runner without a GPU; no one had run the result. Does a native Windows build, made the way that leg makes it, run on Intel GPUs, and does it score like the CPU and like Linux?

## Sources

- Host: Windows 11 Pro 26200, i9-12900K, Arc B580 (driver 32.0.101.9033, Level Zero device 0) and UHD 770 (32.0.101.7092, device 1), confirmed with `sycl-ls`.
- Toolchain: Visual Studio 2022 17.14 (MSVC 14.44), oneAPI 2025.1.1 (`icx-cl`, `icpx`, bundled `ocloc`), Level Zero loader v1.34.0 built from source, Meson 1.12.1 and Ninja 1.13.2 from `requirements/locks/build.txt`, NASM 3.02 (scoop), `xxd` from Git for Windows. The CI leg uses oneAPI 2025.3.0.
- Reference recipe: `windows-gpu-build` in `.github/workflows/libvmaf-build-matrix.yml`.
- Linux references, same revision: CPU-only build (gcc) and SYCL build (`icx`/`icpx`, oneAPI 2026, compute-runtime 26.35) of this branch in `vmaf-dev-mcp:ocloc`, the GPUs through WSL2 `/dev/dxg`.
- Fixtures: Netflix `src01_hrc00/01_576x324` (48 frames); 50 frames of Big Buck Bunny 3840x2160 from `vmaf-bench-fixtures:/f/bbb/`.

## Findings

### The Windows build registered no SYCL kernel

Built as the CI leg builds it (`--default-library=static -Denable_sycl=true -Dcpp_std=c++latest`, `CC=CXX=icx-cl`, `/experimental:c11atomics`), `vmaf --backend sycl` stopped on the first frame:

```text
libvmaf ERROR libvmaf SYCL exception in graph_submit: No kernel named _ZTSZZN12_GLOBAL__N_112launch_reset...CambiSycl... was found
problem reading pictures
```

and 47 of 50 tests of the `sycl` suite failed on the B580 (exit 1 or `0xC0000409`). Meson links MSVC-syntax toolchains with `link.exe` itself (`rule cpp_LINKER: command = "link" ...`). The `-fsycl` that `sycl_dependency` passes to every link became `/fsycl`, which `link.exe` ignores (`LNK4044`, 275 lines per build). `dumpbin /headers` on `integer_cambi_sycl.o` shows why that matters: the host code is ordinary COFF, and the device code sits in `__CLANG_OFFLOAD_BUNDLE__sycl-spir64_gen_image` (144 kB, compiled by `-fno-sycl-rdc`) and `__CLANG_OFFLOAD_BUNDLE__sycl-spir64` sections. `icpx -fsycl -###` on a link shows the driver unbundling both, device-linking the SPIR-V (`llvm-link`, `sycl-post-link`, `llvm-spirv`, `clang-offload-wrapper`) and passing the wrapped objects to `link.exe`. The wrapped image objects define only static symbols plus a `.CRT$XCA` initializer that calls `__sycl_register_lib`. Without the driver nothing is registered: `sycl::get_kernel_ids()` returned 0 kernels.

### What registers them

- `icpx -fsycl -fsycl-link` over the `-fno-sycl-rdc` objects crashes `clang-offload-wrapper` (exception `0xE06D7363`), for all 25 objects and for two.
- Over relocatable-device-code objects (`-fsycl -fsycl-targets=spir64_gen,spir64`, no device codegen at compile time) it works and writes one host object that registers every image, native and SPIR-V (3.7 MB for 18 targets, compressed).
- That object defines no external symbol, so `link.exe` would never pull it out of `vmaf.lib`. `llvm-objcopy --add-symbol` and `--globalize-symbol` are "not supported for COFF". `core/src/sycl/coff_add_anchor.py` appends one external symbol on `.sycl_offloading.descriptor`; `dumpbin /symbols` then lists `vmaf_sycl_device_images` as `External` next to the untouched static symbols, and a `/include` pragma in `common.cpp` pulls the object in.
- Device link time for 18 targets: 355 s serial, 127 s with `-fsycl-max-parallel-link-jobs=8`.
- With the change the runtime reports 96 registered kernels; `test_sycl_kernel_registration` asks for them without a GPU.

### Test results, native Windows build

Through `scripts/ci/run_meson_test.py` (after the runner fix below):

| Suite | Arc B580 (`level_zero:0`) | UHD 770 (`level_zero:1`) |
|---|---|---|
| `sycl` before the fix | 3 OK, 47 FAIL | not run |
| `sycl` | 51/51 OK | 51/51 OK |
| `fast` | 226/226 OK | 226/226 OK |
| whole suite | 239 OK, 1 skipped | not run |

The log line `SYCL: using device: ...` in each test log names the selected GPU.

### Model scores: Windows SYCL against Windows CPU (`--precision max`)

| Pair, model | pooled VMAF CPU | pooled VMAF SYCL (B580 = UHD 770) | pooled Δ | worst per-frame Δ VMAF | worst feature Δ |
|---|---|---|---|---|---|
| 576x324, default (`vmaf_v1.0.16_3d0h`) | 82.81605873421144 | 82.81605793905153 | 7.95e-7 | 8.90e-6 | `integer_motion2_mmxv_18` 1.26e-5 |
| 576x324, `vmaf_v0.6.1` | 76.66783086300089 | 76.667806051359 | 2.48e-5 | 5.92e-5 | `integer_motion2` 1.26e-5 |
| 4K, default | 79.18222768268778 | 79.1822273974861 | 2.85e-7 | 3.94e-6 | `integer_motion2_mmxv_18` 5.56e-6 |
| 4K, `vmaf_v0.6.1` | 79.39225223191019 | 79.3922317284458 | 2.05e-5 | 5.28e-5 | `integer_motion2` 5.56e-6 |

No shared feature exceeds its gate tolerance (5e-5); the `vmaf_v0.6.1` deltas at 576x324 are the ones [the SYCL overview](../backends/sycl/overview.md#numerical-tolerance-vs-the-cpu-scalar-path) records for an A380 on Linux. Both GPUs produce bit-identical output. The CPU run writes a few diagnostic metrics the SYCL twins do not (`VMAF_integer_feature_motion_sad_score`, and for `vmaf_v0.6.1` the VIF numerators/denominators, `integer_adm3`, `integer_aim`).

### Windows against Linux, same revision

- Linux SYCL (container) against Windows SYCL, both GPUs, all four model runs: every feature bit-identical; VMAF differs by at most 6.1e-12 per frame (host-side prediction arithmetic).
- Linux CPU (gcc) against Windows CPU (`icx-cl`): `vmaf_v0.6.1` identical within 5.2e-12; the default model's SpEED-chroma scores differ by up to 2.9e-6 (576x324) and 4.8e-7 (4K), which moves pooled VMAF by 2.7e-8. The SYCL SpEED twins equal the `icx`-built CPU on both OSes, so this is the compiler of the CPU build, not Windows.

### Every SYCL twin on both GPUs

`scripts/ci/cross_backend_parity_gate.py --backends cpu sycl` on both fixtures, both GPUs, 16 gate features, plus `motion_sycl`, `integer_ssim_sycl`, `speed_chroma_sycl` and `speed_temporal_sycl` by name:

| Twin | 576x324 max Δ | 4K max Δ | Tolerance |
|---|---|---|---|
| `adm_sycl` | 1e-6 | 2e-6 | 5e-5 |
| `cambi_sycl` | 0 | 0 | 5e-5 |
| `ciede_sycl` | 1.2e-5 | 9.7e-5 | 5e-3 |
| `float_adm_sycl` | 2.5e-5 | 1.6e-5 | 5e-5 |
| `float_moment_sycl` | 0 | 0 | 5e-5 |
| `float_motion_sycl` | 3e-6 | 2.7e-5 | 5e-5 |
| `float_ms_ssim_sycl` (and `enable_lcs`) | 1e-6 | 1e-6 | 5e-5 |
| `float_psnr_sycl` | 0 | 0 | 5e-5 |
| `float_ssim_sycl` | 1e-6 | rejects auto scale 8; 8.3e-5 at `scale=1` | 5e-5 |
| `float_vif_sycl` | 2.8e-5 | 3e-6 | 5e-5 |
| `integer_ssim_sycl` | 1.5e-8 | 7.8e-8 | 5e-5 |
| `motion_sycl` (shared metrics) | 1.26e-5 | 5.6e-6 | 5e-5 |
| `motion_v2_sycl` | 0 | 0 | 5e-5 |
| `psnr_sycl` | 0 | 0 | 5e-5 |
| `psnr_hvs_sycl` | 8.3e-5 | 8.43e-4 | 5e-4, 3.34e-3 at 4K (ADR-1361) |
| `speed_chroma_sycl`, `speed_temporal_sycl` | 0 | 0 | 5e-5 |
| `ssimulacra2_sycl` | 0 | 0 | 5e-3 |
| `vif_sycl` | 1e-6 | 1e-6 | 5e-5 |

The gate prints `%.6f` scores, so its 1e-6 entries are a rounding floor; the four twins run by name use `--precision max`. B580 and UHD 770 agree to every printed digit. Two gate defects surfaced: the `cambi` cell read `Cambi_feature_cambi_score`, which `vmaf --json` writes as `cambi` (fixed), and the `motion` cell asks both runs for `integer_motion`, which only the SYCL twin emits by default (`T-CI-PARITY-GATE-MOTION-DEBUG-DEFAULT-2026-09-29`). `float_ssim_sycl` at 4K with `scale=1` is 8.3e-5 from the CPU on both GPUs and identical on Linux, so it is the known L·C·S reduction divergence that `scripts/ci/gpu_ulp_calibration.yaml` calibrates to 5e-4 for DG2-G10 only (`T-SYCL-FLOAT-SSIM-XE2-XELP-CALIBRATION-2026-09-29`).

### The Windows CI lanes did not wait for their tests

`meson test` on this host returned before its output ended, with status 0 over failing tests. `scripts/ci/run_meson_test.py` ends in `os.execvp`; on Windows the CRT's `_execvp` starts the program and exits the caller with 0. The `Windows ARM64 MSVC` job of master run `36553546030` logged tests 1 to 17 of 169, failures at 4 (`test_vmaf_per_shot_input`, `0xC0000409`) and 16 (`test_device_target_header_dependencies`), then the next step; `Windows MinGW64` logged 19 of 186 with the same failure at 18. Both jobs are green. With a waiting runner the Windows suite showed four harness failures, all fixed:

| Test | Cause on Windows |
|---|---|
| `test_vmaf_per_shot_input` | reading a `FILE` whose descriptor was closed is an invalid-parameter fast-fail in the Windows CRT |
| `test_device_target_header_dependencies` | Ninja passes `echo ... > f && touch $out` to `CreateProcess`, not a shell |
| `test_vmaf_per_shot` | a native binary cannot open `/dev/zero` (MSYS maps it to `\Device\Null`) or a FIFO |
| `dnn/test_registry`, `dnn/test_cli` | Meson found `System32\bash.exe` (WSL), and `test_registry.sh` spliced a backslash path into Python source |

### Timing (diagnostic; native builds are not canonical bench rows, ADR-1102)

Per frame = `(t(N) - t(2)) / (N - 2)`, minimum of five interleaved runs, `N` = 48 (576x324) or 50 (4K), fixtures in the page cache. Other agents shared the GPUs through WSL during the session; minima keep most of that out.

| Model, fixture | CPU 1 thread | CPU 16 threads | Windows B580 | Windows UHD 770 | Linux container B580 | Linux container UHD 770 |
|---|---|---|---|---|---|---|
| default, 576x324 | 2.48 ms | 0.51 ms | 2.56 ms | 5.30 ms | 2.38 ms | 10.56 ms |
| default, 4K | 77.5 ms | 20.4 ms | 44.8 ms | 66.9 ms | 52.3 ms | 76.1 ms |
| `vmaf_v0.6.1`, 576x324 | 5.21 ms | 0.57 ms | 0.72 ms | 5.09 ms | 1.14 ms | 6.85 ms |
| `vmaf_v0.6.1`, 4K | 169.6 ms | 20.8 ms | 9.32 ms | 130.6 ms | 8.92 ms | 139.0 ms |

The Linux columns are the same revision with the fixtures copied into the container; read from the Windows bind mount they were 5 to 20 times slower (I/O bound). Windows is not slower than the container on either GPU. With the default model the B580 spends most of a 4K frame on the ADM branch, which runs on the CPU because the SYCL twin has no AIM pass (`T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05`); the internal SYCL timer reports 7.9 ms GPU against 38.5 ms CPU per frame there, and 9.2 ms GPU for all of `vmaf_v0.6.1`.

### Host friction worth documenting

- `setvars.bat` failed for every component when `NoDefaultCurrentDirectoryInExePath` was set (as in this agent's shell): it `pushd`s into each component and `call`s `vars.bat` from the current directory.
- The Level Zero loader's CMake adds `/Qspectre`; the Visual Studio generator then fails with `MSB8040` unless the Spectre libraries are installed. The Ninja generator builds it.
- oneAPI 2025.1.1's `ocloc` does not know `bmg-g31` (`Unknown acronym`); the build used the default list without it. CI's 2025.3.0 knows it.
- Without `xxd` on `PATH`, configure disables the built-in models silently (`Program xxd found: NO`).

## Alternatives explored

See the ADR-1364 decision matrix: linking through the driver (Meson cannot), `-fsycl-link` on `-fno-sycl-rdc` objects (wrapper crash), replaying the driver's device-link steps in Meson (too coupled to driver internals), passing the object on every link line (leaves `vmaf.lib` consumers broken), `/WHOLEARCHIVE` and a DLL.

## Open questions

- The device link was run on oneAPI 2025.1.1 only; the CI leg (2025.3.0) builds it, and its registration step checks the kernel count, but no GPU has run a 2025.3.0 Windows build.
- The two Windows test lanes now gate their whole suites for the first time; failures specific to MinGW-w64 gcc or the ARM64 MSVC toolset were not reproducible on this host.

## Reproduce

```bat
rem after the environment in docs/backends/sycl/windows.md
meson setup build core --buildtype release --default-library=static -Denable_float=true -Dcpp_std=c++latest -Denable_cuda=false -Denable_sycl=true
ninja -C build
set ONEAPI_DEVICE_SELECTOR=level_zero:0
python scripts\ci\run_meson_test.py -- -C build --suite sycl --print-errorlogs
build\tools\vmaf.exe -r src01_hrc00_576x324.yuv -d src01_hrc01_576x324.yuv -w 576 -h 324 -p 420 -b 8 --backend sycl --precision max --json -o b580.json
python scripts\ci\cross_backend_parity_gate.py --vmaf-binary build\tools\vmaf.exe --reference src01_hrc00_576x324.yuv --distorted src01_hrc01_576x324.yuv --width 576 --height 324 --backends cpu sycl --features adm cambi ciede float_adm float_moment float_motion float_ms_ssim float_ms_ssim_lcs float_psnr float_ssim float_vif motion_v2 psnr psnr_hvs ssimulacra2 vif
```

## Related

- [ADR-1364](../adr/1364-windows-sycl-msvc-device-link.md), [ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md), [ADR-0121](../adr/0121-windows-gpu-build-only-legs.md), [ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md), [ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md).
- [SYCL on Windows](../backends/sycl/windows.md).
- State rows: `T-SYCL-WINDOWS-MSVC-KERNELS-UNREGISTERED-2026-09-29`, `T-CI-WINDOWS-MESON-TEST-RUNNER-EXIT-0-2026-09-29`, `T-TEST-WINDOWS-HARNESS-MASKED-FAILURES-2026-09-29`, `T-CI-PARITY-GATE-CAMBI-KEY-2026-09-29`, `T-CI-PARITY-GATE-MOTION-DEBUG-DEFAULT-2026-09-29`, `T-SYCL-FLOAT-SSIM-XE2-XELP-CALIBRATION-2026-09-29`.
