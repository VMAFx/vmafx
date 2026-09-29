<!-- markdownlint-disable MD013 MD060 -->
# Research-2124: SYCL AOT images dropped at the link — 2026-09-29

- **Status**: Active
- **Workstream**: RC3 performance ([ADR-1341](../adr/1341-rc-correctness-benchmark-retrain-sequence.md), [ADR-1352](../adr/1352-rc-phase-shift-plus-one.md)); decision in [ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md)
- **Last updated**: 2026-09-29

## Question

`sycl_icpx_aot_targets` ([ADR-0568](../adr/0568-sycl-icpx-aot-targets-default.md)) asks for native Intel GPU images for 19 targets, and configure says so, yet the dev container's `libvmaf.so.3` carries only a `sycl-spir64` bundle and every process JIT-compiles its kernels. Where do the images go, what does it take to keep them, and what does AOT buy?

## Sources

- `core/src/meson.build`: `sycl_toolchain_args`, `sycl_common_args`, `sycl_feature_args`, `sycl_dependency` (`link_args : ['-fsycl']`).
- oneAPI DPC++ 2026.1.1 (`icpx --version`), compute-runtime 26.35.39758.10 with IGC 2.41.5 (`intel-ocloc`, `intel-igc-core-2`), in `vmaf-dev-mcp:local`.
- Intel DPC++ users manual on `-fsycl-targets` and `-fsycl-rdc`, and the AOT guide ([ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md) references).
- Host: Intel i9-12900K, Arc B580 (`bmg-g21`, Level Zero device 0) and UHD 770 (`adl-s`, device 1) through WSL2 `/dev/dxg`.

## Findings

### The link builds only the targets on its own command line

A one-kernel reproducer shows the whole mechanism (`k.cpp`, one `parallel_for`):

| Step | Offload bundles in the output |
|---|---|
| `icpx -fsycl -fsycl-targets=spir64_gen,spir64 -Xsycl-target-backend=spir64_gen '-device bmg-g21,adl-s' -c` | `sycl-spir64_gen`, `sycl-spir64`, host (bitcode) |
| link the object with `icpx -fsycl -shared` (what meson does) | `sycl-spir64` only |
| link with the same target flags | `sycl-spir64_gen`, `sycl-spir64` |
| compile with `-fno-sycl-rdc` plus the target flags, link with plain `-fsycl` | `sycl-spir64_gen_image` in the object; `sycl-spir64_gen`, `sycl-spir64` in the library |

With relocatable device code, the icpx default, the compile step stores bitcode and the link does all device code generation for the targets the link names. Meson links with `-fsycl` alone, so the `spir64_gen` bitcode was dropped. Without `ocloc` on `PATH` either target-flag variant fails: "ocloc tool could not be found and is required for AOT compilation". The Linux oneAPI compiler does not ship ocloc, and neither the dev container, the production oneAPI builder nor any CI SYCL leg installed it; the only copy in the image is VTune's private one under `GTPin`.

### Link-time AOT is not viable here

Relinking the existing `libvmaf.so` with the target flags ran device code generation for all 24 SYCL TUs in one single-threaded ocloc pass: 167 s, then ocloc aborted in `integer_psnr_hvs_sycl` ("longjmp causes uninitialized stack frame") and the link failed. The 113 test executables that link `libvmaf.a` with `-fsycl` would each redo that work.

### Per-TU compile-time AOT, and one TU IGC cannot compile

With `-fno-sycl-rdc` the AOT work moves to each TU's compile, runs in parallel, and every link keeps the images. Compiling `integer_psnr_hvs_sycl.cpp` once per target: 16 targets pass, `lnl-m`, `bmg-g21` and `bmg-g31` (Xe2) abort ocloc with the longjmp message. That is the same IGC fault that makes the SPIR-V JIT segfault `psnr_hvs_sycl` on the B580 at run time (`test_sycl_psnr_hvs_parity` and `_large` die with SIGSEGV on master too). The reworked kernel on `fix/sycl-b580-psnr-hvs-adm-tiny` compiles for all three Xe2 targets under ocloc 26.35.

### Size and build time

| Build (19 targets, `-j6`, 22-thread host) | Clean `ninja` | `libvmaf.so` | `test_sycl` | Build directory |
|---|---|---|---|---|
| master, JIT only (images dropped) | 82 s | 4.57 MB | n/a | 557 MB |
| `-fno-sycl-rdc` | 151 s | 65.1 MB | 64.0 MB | 11 GB |
| `-fno-sycl-rdc --offload-compress` | 162 s | 7.69 MB | 6.63 MB | 1.1 GB |

Uncompressed, the native ISA for 19 targets is 60 MB per binary; zstd brings the `spir64_gen` section to 3.07 MB. The default list names 19 acronyms but 13 distinct GFX IP versions (`dg2-g10` and `acm-g10` are one IP, for example); ocloc compiles each acronym anyway.

### Startup and steady state

`vmaf` on the Netflix pair (576x324, 8-bit 4:2:0), `--backend sycl`, median of 5, builds interleaved per repetition. "Cold" sets `NEO_CACHE_PERSISTENT=0` and `SYCL_CACHE_PERSISTENT=0`: the state of a fresh container, a first run after install or driver update, or a host without a writable cache directory.

| Device | Case | JIT (master) | AOT | AOT compressed |
|---|---|---|---|---|
| B580 | default model, 1 frame | 524 ms | 203 ms | 201 ms |
| B580 | `--feature psnr_sycl`, 1 frame | 206 ms | 128 ms | 123 ms |
| B580 | 46 frames, `t(48) - t(2)` | 519 ms | 534 ms | 520 ms |
| UHD 770 | default model, 1 frame | 609 ms | 255 ms | 245 ms |
| UHD 770 | `--feature psnr_sycl`, 1 frame | 129 ms | 92 ms | 87 ms |
| UHD 770 | 46 frames, `t(48) - t(2)` | 6440 ms | 6104 ms | 6325 ms |

With a warm compiler cache the JIT build matched AOT (B580 default model 192 against 194 ms, UHD 770 231 against 229 ms), so AOT buys the cold start and nothing per frame. The UHD 770 steady state moves by several percent between runs because the iGPU shares the package power budget with the CPU; the interleaving keeps that drift out of the comparison but not out of the absolute numbers.

### Numerics

Per-frame JSON at `--precision max`, JIT build against the compressed AOT build, 48 frames, both devices: the default model and every SYCL extractor (`adm`, `cambi`, `ciede`, `float_adm`, `float_moment`, `float_motion`, `float_ms_ssim`, `float_psnr`, `float_ssim`, `float_vif`, `integer_ssim`, `motion`, `motion_v2`, `psnr_hvs`, `psnr`, `speed_chroma`, `speed_temporal`, `ssimulacra2`, `vif`) are bit-identical: 7104 values, no difference. `psnr_hvs_sycl` on the B580 crashes in both builds (the IGC fault above), so it has no B580 comparison.

The SYCL test suite (`--suite sycl`, 49 tests) gives the same result for both builds: 48 pass on the UHD 770 and 46 on the B580. The failures are the same in both: `test_sycl_adm_tiny_frames` on both devices and the two `psnr_hvs` parity tests on the B580, all open on `fix/sycl-b580-psnr-hvs-adm-tiny`.

### Production oneAPI image

`docker/Dockerfile.production-gpu --target final-oneapi2025` builds with the change: the `intel/oneapi-basekit` 2025.3.2 builder installs ocloc through `scripts/ci/install-intel-ocloc.sh`, icpx 2025.3.2 compiles all 19 targets, and the image check passes under the image's Python 3.12 through the `zstd` command-line fallback. The `intel/oneapi-runtime` 2025.3.1 runtime loads the compressed images: on the UHD 770 the default model's first two frames take 240 ms against 761 ms for the master image (one cold run each), with the same score (92.632448). On the B580 both images segfault on the first kernel (`T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29`), which predates this change; that runtime carries compute-runtime 25.18 and IGC 2.11.

## Reproducer

```bash
# In vmaf-dev-mcp with ocloc (scripts/ci/install-intel-ocloc.sh):
CC=icx CXX=icpx meson setup build core -Denable_sycl=true -Denable_cuda=false -Db_lto=false
ninja -C build                                   # runs src/sycl_aot_image_check
clang-offload-bundler --list --type=o --input=build/src/libvmaf.so   # sycl-spir64_gen, sycl-spir64
NEO_CACHE_PERSISTENT=0 SYCL_CACHE_PERSISTENT=0 ONEAPI_DEVICE_SELECTOR=level_zero:0 \
  build/tools/vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 --backend sycl --frame_cnt 1 -q
```
