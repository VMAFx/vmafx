<!-- markdownlint-disable MD060 -->
# HIP (AMD ROCm) compute backend

The HIP backend runs VMAFx feature extractors on AMD GPUs through ROCm.
Build with `-Denable_hip=true -Denable_hipcc=true`, then run
`vmaf --backend hip`. This page covers building, running, what is
implemented and what is still open. Per-twin detail is on the pages listed
under [More HIP pages](#more-hip-pages).

!!! note "Status"
    All 19 registered HIP extractors run on AMD hardware. 18 of them return
    the CPU extractor's values bit for bit, and `ciede_hip` is within `1e-9`
    of the CPU. The state ledger [`docs/state.md`](../../state.md) holds
    every open item, and the dated change record is on the
    [history page](history.md). All measurements on these pages come from
    one integrated GPU (gfx1036); a discrete AMD GPU has not been measured.

## Requirements

| Item | Value |
|---|---|
| ROCm | 7.0 or later builds; **10.1.0** is the version tested in CI, in the dev container and in the published GPU images (`build-config.env` `ROCM_VERSION`, [ADR-1225](../../adr/1225-rocm-10-therock-migration.md)) |
| Libraries | `libamdhip64` and `<hip/hip_runtime_api.h>`, found through the `hip-lang` package, `HIP_PATH`, or `/opt/rocm` |
| Compiler | `hipcc` in `PATH` when `enable_hipcc=true` |
| Hardware | An AMD GPU visible to ROCm; the default fat binary targets gfx90a, gfx1030, gfx1036 and gfx1100 |

ROCm 10 has no apt channel. Since ROCm 7.14 AMD builds and releases through
"TheRock", and `repo.radeon.com/rocm/apt/` ends at 7.2.4. VMAFx therefore
installs ROCm from the digest-pinned `rocm/dev-ubuntu-26.04:10.1.0-full`
container image. On a workstation, use your distribution's ROCm packages
(any 7.0+ release builds) or the dev container. CI runs
`scripts/ci/install-rocm-from-image.sh`, which streams the image's
`/opt/rocm` out of the registry without a 29 GB `docker pull`.

## Build

1. Configure from the repository root. The Meson source directory is `core/`.

    ```bash
    meson setup build core -Denable_cuda=false -Denable_sycl=false \
                           -Denable_hip=true -Denable_hipcc=true
    ```

2. Compile.

    ```bash
    ninja -C build
    ```

3. Run the test suite. Device tests skip with exit 77 when no AMD device is
   visible.

    ```bash
    python3 scripts/ci/run_meson_test.py -- -C build
    ```

### Build options

| Option | Default | Effect |
|---|---|---|
| `enable_hip` | `false` | Compiles the HIP host runtime. With it off, every public `libvmaf_hip.h` entry point returns `-ENOSYS`. |
| `enable_hipcc` | `false` | Compiles the device kernels with `hipcc` and embeds the HSACO code objects. Without it, every extractor returns `-ENOSYS` at `init()`; `float_ssim_hip`, `integer_ssim_hip` and `vmaf_hip_picture_alloc` log an error that names `-Denable_hipcc=true`. Requires `enable_hip=true`. |
| `hip_gfx_targets` | empty (auto-detect) | Comma-separated `--offload-arch` list for the HSACO fat binary. See [GFX targets](#gfx-targets). |
| `enable_float_vif_hip_autodispatch` | `true` | Sets `VMAF_FEATURE_EXTRACTOR_HIP` on `float_vif_hip`, so `--backend hip` and a VMAFx context on a HIP device select it for `float_vif` ([ADR-0623](../../adr/0623-scaffold-audit-p2-half-finished.md); on by default since [ADR-2092](../../adr/2092-vmafx-hip-device-frames.md)). Off: the twin runs only when named. |
| `compress_device_code` | `true` | Stores each kernel's code object bundle compressed (`hipcc --offload-compress --offload-compression-level=22`, a zstd `CCOB` bundle): 1.09 MB instead of 17.9 MB for the 25 tester targets. The HIP runtime decompresses a bundle when `hipModuleLoadData()` loads it (measured with the ROCm 7.2.4 runtime on a gfx1036: no change in load time); the AMD tester image ships the runtime of the ROCm that compiled its kernels. See [`compress_device_code`](../../development/build-flags.md#compress_device_code). |

Pre-compiled HSACO fat binaries are not bundled without `hipcc`, because ROCm
needs target-specific code objects. The CI compile lane (`Ubuntu HIP`) builds
with `-Denable_hipcc=false`, and no GitHub-hosted runner has an AMD GPU, so
CI does not compile or exercise the kernels. HIP runtime types
(`hipDevice_t`, `hipStream_t`) cross the public ABI as `uintptr_t`, which
keeps `libvmaf_hip.h` free of `<hip/hip_runtime.h>`.

### GFX targets

`hipcc --genco` produces one HSACO blob per `--offload-arch` target. Meson
resolves the target list in this order:

1. The `-Dhip_gfx_targets=<csv>` override.
2. `rocm_agent_enumerator` (the `gfx*` lines).
3. `hipconfig --amdgpu-target`.
4. The fallback list `gfx90a,gfx1030,gfx1036,gfx1100` (CDNA2 server, RDNA2
   desktop, the Raphael APU iGPU and RDNA3).

Steps 2 and 3 succeed only when the build host can see a GPU. In a no-GPU
sandbox (BuildKit, CI) both return nothing and the build uses step 4. The
`HIP HSACO targets:` line of the Meson configure output shows the resolved
list.

To shrink the fat binary, pin one target or a comma-separated list:

```bash
meson setup build core -Denable_hip=true -Denable_hipcc=true \
                       -Dhip_gfx_targets=gfx1036
```

```bash
meson setup build core -Denable_hip=true -Denable_hipcc=true \
                       -Dhip_gfx_targets=gfx90a,gfx1100
```

`rocm_agent_enumerator` prints the target of the installed GPU.

!!! note "Why the fallback list is wide"
    The fallback was `gfx90a` only until
    [ADR-0561](../../adr/0561-hip-gfx-targets-fallback-widening.md). That
    narrow list shipped `libvmaf.so` binaries that failed at runtime on the
    fork's own dev host (a Raphael APU, gfx1036) with
    `hip_fatbin.cpp: No compatible code objects found for: gfx1030`. Under
    ROCm 6.x and 7.x that host needed `HSA_OVERRIDE_GFX_VERSION=10.3.0` to
    alias gfx1036 onto the allowlisted gfx1030. ROCm 10 supports gfx1036
    natively, so the override is gone (ADR-1225).

## Run

`--backend hip` selects the HIP twin of every feature the model or the
`--feature` list needs, and runs the rest on the CPU. `--backend hip` pins
device 0; `--hip_device N` picks another device by ordinal.

```bash
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
     --backend hip --json --output out.json
```

The JSON output names the extractor that ran for each feature under
`feature_backends`, and the backend that ran under `backend_used`.

| Flag | Effect |
|---|---|
| `--backend hip` | Exclusive HIP selection; disables the other backends before dispatch. An explicit request for a backend that was not compiled in fails with exit code 100. |
| `--hip_device N` | Selects the HIP GPU by ordinal; every HIP twin runs on it. An ordinal the runtime does not have fails with exit code 100 under `--backend hip`. |
| `--no_hip` | Forbids HIP dispatch even when the backend is built in. |
| `--feature NAME` | Runs an extractor. A CPU name runs the HIP twin under `--backend hip`; the twin's own name (`psnr_hip`) always runs it. |

[CLI reference](../../usage/cli.md) | [backend selection](../index.md)

A twin that carries no HIP flag runs only when you name it. In a default
build every twin carries it; a build with
`-Denable_float_vif_hip_autodispatch=false` leaves `float_vif_hip` without
it, so it needs the `_hip` name there:

```bash
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
     --backend hip --feature float_vif_hip --no_prediction --json --output out.json
```

A twin named this way runs on the thread that calls `vmaf_read_pictures()`,
with or without `--threads`.

FFmpeg selects the device with the `hip_device=N` filter option (patch
`0011-libvmaf-wire-hip-backend-selector.patch` in `ffmpeg-patches/`,
[ADR-0380](../../adr/0380-ffmpeg-patches-hip-backend-selector.md)).

A program that already holds its frames on the GPU (a decoder's dma-bufs, a
renderer's GL textures, HIP device memory) hands them to the VMAFx API
instead of `vmaf_read_pictures()`: see
[HIP devices](../../api/vmafx/index.md#hip-devices). On a host with a HIP
device, the import is checked against host uploads by the `hip` suite:

```bash
python3 scripts/ci/run_meson_test.py -- -C build --suite hip \
    test_vmafx_import_hip test_vmafx_import_hip_bitexact \
    test_vmafx_import_hip_fence test_vmafx_import_hip_gl
```

The bit-exactness run prints the cells, values and imports it compared and
the host copies it counted (0), and repeats a cell that differs, printing
each repeat (see [the gfx1036 loses stream commands](#the-gfx1036-loses-stream-commands)).

The HIP backend reads no dispatch environment variable: every HIP twin
submits directly, and `--backend hip` picks a twin by its registration flag.
`VMAF_HIP_DISPATCH`, which earlier pages listed, was read by a function
nothing called and has been removed
([ADR-1571](../../adr/1571-gpu-dispatch-env-consulted.md)).

## Registered extractors

Nineteen extractors are registered in `core/src/feature/feature_extractor.cpp`.
"Named only" means the extractor carries no `VMAF_FEATURE_EXTRACTOR_HIP`
flag, so `--backend hip` does not pick it for the CPU name.

| Registered name | CPU feature | `--backend hip` picks it | Emits | Added in |
|---|---|---|---|---|
| `psnr_hip` | `psnr` | yes | `psnr_y`, `psnr_cb`, `psnr_cr` | [ADR-0241](../../adr/0241-hip-first-consumer-psnr.md) |
| `float_psnr_hip` | `float_psnr` | yes | `float_psnr` | [ADR-0254](../../adr/0254-hip-second-consumer-float-psnr.md) |
| `ciede_hip` | `ciede` | yes | `ciede2000` | [ADR-0259](../../adr/0259-hip-third-consumer-ciede.md), PR #1016, [ADR-1448](../../adr/1448-hip-ciede-cpu-arithmetic.md) |
| `float_moment_hip` | `float_moment` | yes | four `float_moment_*` | [ADR-0260](../../adr/0260-hip-fourth-consumer-float-moment.md) |
| `motion_v2_hip` | `motion_v2` | yes | `motion_v2` SAD, `motion2_v2`, `motion3_v2` | [ADR-0267](../../adr/0267-hip-sixth-consumer-motion-v2.md) |
| `motion_hip` | `motion` | yes | `motion`, `motion2`, `motion3` | [ADR-0523](../../adr/0523-hip-integer-motion-extractor-registration.md), PR #1004 |
| `float_motion_hip` | `float_motion` | yes | `motion`, `motion2`, `motion3` | [ADR-0373](../../adr/0373-hip-batch2-float-motion.md) |
| `float_ssim_hip` | `float_ssim` | yes | `float_ssim` (and L, C, S with `enable_lcs`) | [ADR-0375](../../adr/0375-hip-batch3-float-moment-float-ssim.md) |
| `float_vif_hip` | `float_vif` | yes (named only with `-Denable_float_vif_hip_autodispatch=false`) | `float_vif_scale0..3` | ADR-0379, [ADR-0592](../../adr/0592-hip-float-vif-stub-removal.md) |
| `float_adm_hip` | `float_adm` | yes | `adm2`, `adm_scale0..3`, `aim`, `adm3` | [ADR-0468](../../adr/0468-hip-float-adm-real-kernel.md), PR #1024, [ADR-1458](../../adr/1458-hip-float-adm-cpu-arithmetic.md) |
| `psnr_hvs_hip` | `psnr_hvs` | yes | `psnr_hvs` and per-channel values | PR #995 |
| `cambi_hip` | `cambi` | yes | `cambi` | PR #996, [ADR-1378](../../adr/1378-hip-cambi-device-resident.md) |
| `ssimulacra2_hip` | `ssimulacra2` | yes | `ssimulacra2` | PR #1000 |
| `vif_hip` | `vif` | yes | `vif_scale0..3` | PR #1001 |
| `adm_hip` | `adm` | yes | `integer_adm2`, `integer_aim`, `integer_adm3`, `integer_adm_scale0..3` | PR #1007, [ADR-1423](../../adr/1423-hip-adm-cpu-row-rounding.md), [ADR-1525](../../adr/1525-adm-hip-aim-device-pass.md) |
| `integer_ms_ssim_hip` | `float_ms_ssim` | yes | `float_ms_ssim` (and `_cb`, `_cr`, L, C, S) | PR #1013 |
| `integer_ssim_hip` | `ssim` | yes | `ssim` | PR #999, [ADR-0564](../../adr/0564-integer-ssim-gpu-real-kernels.md) |
| `speed_chroma_hip` | `speed_chroma` | yes | SpEED chroma features | [ADR-0567](../../adr/0567-speed-chroma-temporal-real-gpu.md), [ADR-0852](../../adr/0852-hip-speed-extractor-wiring.md), [ADR-1384](../../adr/1384-hip-speed-device-resident.md) |
| `speed_temporal_hip` | `speed_temporal` | yes | SpEED temporal features | ADR-0567, ADR-0852, ADR-1384 |

The kernel file, algorithm and per-twin notes for each row are in
[HIP twins](twins.md#kernel-notes).

## Agreement with the CPU

18 twins return the CPU extractor's values bit for bit. A twin is declared
exact by one file `scripts/ci/exact_twins.d/<feature>.hip`, and the
[cross-backend parity gate](../../development/cross-backend-gate.md) then
compares it with tolerance 0 ([generated list of exact
twins](../../development/cross-backend-exact-twins.md)). The one bounded
twin takes its bound from `LIBM_TWINS` in
`scripts/ci/cross_backend_calibration.py`.

The table is the state measured on a gfx1036 (ROCm 7.2.4, glibc 2.44) at
`--precision max` against `--backend cpu`: 110 frames of typical content (the
Netflix 576x324 pair at 8 and 10 bits, both 1920x1080 checkerboard pairs,
Sparks 480x270 at 10 bits, 48 frames of BBB 3840x2160) and 68 frames that
stress the arithmetic (12 and 16 bits, 10-bit 4:2:2, full-range noise at four
depths, a bright 16-bit 1080p pair).

"Values identical" counts every output of
every frame; the `speed_*` rows come from a wider set of clips
([Research-1437](../../research/1437-hip-twin-exactness-sweep.md) has the
sweep per output and per fixture). "Largest difference before" is the
difference measured when the sweep first ran (2026-10-01, when eight of these
twins were exact), before the twin took the CPU's arithmetic.

| CPU feature | HIP twin | Values identical | Largest difference | Before | Exact twin |
|---|---|---|---|---|---|
| `motion` (also `debug=true`) | `motion_hip` | 534 of 534 (712 of 712) | 0 | 0 | yes ([ADR-1437](../../adr/1437-hip-exact-twins-declared.md)) |
| `motion_v2` | `motion_v2_hip` | 534 of 534 | 0 | 0 | yes (ADR-1437) |
| `psnr` | `psnr_hip` | 534 of 534 | 0 | 0 | yes (ADR-1437) |
| `float_ms_ssim` (also `enable_lcs`) | `integer_ms_ssim_hip` | 178 of 178 (2848 of 2848) | 0 | 0 | yes (ADR-1437) |
| `cambi` | `cambi_hip` | 178 of 178 | 0 | 0 | yes (ADR-1437) |
| `adm` (also `debug=true`) | `adm_hip` | 890 of 890 (4141 of 4141 with `aim` and `adm3`) | 0 | 0 | yes ([ADR-1423](../../adr/1423-hip-adm-cpu-row-rounding.md), [ADR-1525](../../adr/1525-adm-hip-aim-device-pass.md)) |
| `float_motion` | `float_motion_hip` | 534 of 534 | 0 | 0 | yes ([ADR-1419](../../adr/1419-hip-float-motion-cpu-float-sum.md)) |
| `psnr_hvs` | `psnr_hvs_hip` | 680 of 680 (8 to 12 bits) | 0 | 0 | yes ([ADR-1401](../../adr/1401-psnr-hvs-sycl-hip-exact-twins.md)) |
| `vif` | `vif_hip` | 712 of 712 | 0 | 5.4e-7 | yes ([ADR-1435](../../adr/1435-hip-vif-cpu-log2-table.md)) |
| `ssim` | `integer_ssim_hip` | 178 of 178 | 0 | 1.1e-11 | yes ([ADR-1438](../../adr/1438-hip-ssim-cpu-frame-sum.md)) |
| `float_psnr` | `float_psnr_hip` | 178 of 178 | 0 | 7.6e-8 dB | yes ([ADR-1440](../../adr/1440-hip-float-psnr-exact-block-sums.md), [ADR-1499](../../adr/1499-float-psnr-twins-cpu-row-order.md)) |
| `float_ssim` (also `enable_lcs`) | `float_ssim_hip` | 178 of 178 (712 of 712) | 0 | 5.4e-7 | yes ([ADR-1441](../../adr/1441-hip-float-ssim-cpu-window-sums.md)) |
| `float_vif` | `float_vif_hip` | 712 of 712 | 0 | 1.1e-4 | yes ([ADR-1444](../../adr/1444-hip-float-vif-cpu-arithmetic.md)) |
| `ssimulacra2` | `ssimulacra2_hip` | 178 of 178 | 0 | 7.6e-11 | yes ([ADR-1445](../../adr/1445-hip-ssimulacra2-cpu-sum-order.md)) |
| `float_moment` | `float_moment_hip` | 712 of 712 | 0 | 1.0e-4 | yes ([ADR-1447](../../adr/1447-hip-float-moment-cpu-float-squares.md), [ADR-1497](../../adr/1497-float-moment-twins-cpu-sum-past-2-53.md)) |
| `float_adm` (also `debug=true`) | `float_adm_hip` | 1246 of 1246 (3204 of 3204) | 0 | 1.3e-5 | yes ([ADR-1458](../../adr/1458-hip-float-adm-cpu-arithmetic.md)) |
| `speed_chroma` | `speed_chroma_hip` | 759 of 759 | 0 | 1.4e-6 | yes ([ADR-1477](../../adr/1477-speed-upstream-double-math.md)) |
| `speed_temporal` | `speed_temporal_hip` | 256 of 256 | 0 | 4.8e-7 | yes (ADR-1477) |
| `ciede` | `ciede_hip` | 115 of 178 | 1.4e-11 | 1.1e-5 | no: the C library's `powf` and the last bits of an fp32 pair, bounded at `1e-9` ([ADR-1448](../../adr/1448-hip-ciede-cpu-arithmetic.md)) |

Exact twins cost time. [Cost of exactness](twins.md#cost-of-exactness) lists
the frame time before and after each twin took the CPU's arithmetic. To
re-run the comparison:

```bash
python3 scripts/ci/run_meson_test.py -- -C build-hip test_hip_exact_twins
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build-hip/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --backends cpu hip \
    --features vif motion motion_debug motion_v2 adm psnr float_moment ssim \
        float_ssim float_ssim_lcs float_ms_ssim float_ms_ssim_lcs float_psnr \
        float_motion float_vif float_adm psnr_hvs ssimulacra2 cambi ciede \
        speed_chroma
```

## Known gaps

Open items only; the ledger row ids are in
[`docs/state.md`](../../state.md).

- **`adm_hip` is slower than the CPU on an integrated GPU.** On the gfx1036
  of the measuring host (2 compute units) the twin takes about 220 ms per
  3840x2160 frame, the CPU `adm` extractor 15 ms with 16 threads; the AIM
  pass recomputes csf(r) at all nine taps of every threshold, as the CUDA
  twin does, and is two thirds of that. The default model under
  `--backend hip` goes from 65 to 273 ms per 3840x2160 frame because its ADM
  now runs on that device (`T-HIP-ADM-AIM-INLINE-COST-2026-10-04`, RC8).
- **Imported frames are copied once more on the device.** The VMAFx API
  imports device pointers, dma-bufs, arrays and GL textures without a host
  copy, but the twins copy each imported frame into their own buffers on the
  device's library stream, where the CUDA twins read the picture itself
  ([ADR-2092](../../adr/2092-vmafx-hip-device-frames.md); RC8 tuning row
  `T-HIP-IMPORT-TWIN-DEVICE-COPY-2026-10-06`). See
  [picture uploads](uploads.md#zero-copy-import).
- **Import limits of the runtime.** A sync_file is an acquire fence only,
  checked on the host: ROCm 10.1 imports no external semaphore that could
  carry one (ROCm 7.2.4 aborts the process trying), so no HIP stream waits on
  or signals one (`T-HIP-ROCM-NO-SYNC-FILE-SEMAPHORE-2026-10-06`). OpenGL
  textures are imported through EGL's dma-buf export from the current EGL
  context, because the runtime's own GL interop cannot read a texture with
  ROCm 10.1 ([ADR-2132](../../adr/2132-hip-gl-textures-through-egl-dmabuf.md));
  a tiled export is copied on the GPU and needs
  `VMAFX_IMPORT_ALLOW_COPY`. A HIP device has no frame pools.
- **Six twins stage their own copy of the frame** instead of reading the
  shared planes: `integer_ms_ssim_hip`, `psnr_hvs_hip`, `cambi_hip`,
  `speed_chroma_hip`, `speed_temporal_hip` and `ssimulacra2_hip`
  (`T-HIP-SHARED-FRAME-REMAINING-TWINS-2026-10-01`, RC8).
- **Exact twins are slower than before, and some are slower than the CPU on
  the gfx1036.** Open tuning rows, all RC8 and all with correct scores:
  `T-HIP-FLOAT-SSIM-EXACT-THROUGHPUT-2026-10-01`,
  `T-HIP-FLOAT-VIF-EXACT-THROUGHPUT-2026-10-02`,
  `T-HIP-SSIMULACRA2-EXACT-THROUGHPUT-2026-10-02`,
  `T-HIP-CIEDE-EXACT-THROUGHPUT-2026-10-02`,
  `T-HIP-FLOAT-MS-SSIM-EXACT-THROUGHPUT-2026-10-02`,
  `T-SYCL-HIP-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01` and
  `T-GPU-FLOAT-MOMENT-EXACT-SUM-COST-2026-10-03`.
- **No CI device.** CI compiles the host code without `hipcc`; the kernels
  are exercised only on a developer's AMD GPU.

### The gfx1036 loses stream commands

This is a platform defect, deferred as
`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`. On a gfx1036 (ROCm 7.2.4,
Linux 7.2.8) a HIP stream now and then never runs a run of the commands it
was given, roughly once per $10^{4}$ frames, on master as well. A HIP twin then
reports a wrong score for that frame: `vif_hip` reports the sums of two frames
when the memset of its accumulators is lost, or fails the run with
`invalid ratio` when a scale's kernel is lost.

Nothing in VMAFx sets it off,
and no runtime setting tried stops it.

To check a device or a driver update, run the probe that reproduces it
without VMAFx:

```bash
hipcc -O2 --offload-arch=gfx1036 scripts/dev/hip_dispatch_drop_probe.hip -o /tmp/probe
for i in 1 2 3 4 5; do /tmp/probe 100000 12 0; done
```

Each run prints `bad_frames` and `lost` (dispatches that never ran); both are
0 on a healthy stack. On the gfx1036 five runs gave 55 bad frames in 500000
and 382 lost dispatches in 6.0 million; on Linux 7.2.9 three runs of 20000
frames gave 0, 17 and 3 bad frames (2026-10-06, load average 24 to 38).
Until a driver update clears it, compare HIP scores from this device over
repeated runs and treat a single-frame mismatch as suspect, not as a code
defect.

A separate, deferred report (`T-HIP-GFX1036-SDMA-READ-FAULT-2026-10-01`) is
one `psnr_hvs_hip` run killed by a GPU memory access fault raised by the copy
engine; 131 further runs of the same command were clean.

## More HIP pages

| Page | Content |
|---|---|
| [HIP twins](twins.md) | Kernel notes, floating-point policy, the cost of exactness and one section per twin with its measurements |
| [Picture uploads and device state](uploads.md) | Shared frame planes, zero-copy status, accumulator clearing |
| [History](history.md) | Dated status entries and the ADR-0537, ADR-0539 and ADR-1103 bring-up |
| [Backend selection](../index.md) | How the backends compete and how `--backend` resolves |

## References

- [ADR-0212](../../adr/0212-hip-backend-scaffold.md) — the original scaffold.
- [ADR-0241](../../adr/0241-hip-first-consumer-psnr.md) — first consumer
  (`psnr_hip`).
- [ADR-0254](../../adr/0254-hip-second-consumer-float-psnr.md) — second
  consumer (`float_psnr_hip`).
- [ADR-0259](../../adr/0259-hip-third-consumer-ciede.md) — third consumer.
- [ADR-0260](../../adr/0260-hip-fourth-consumer-float-moment.md) — fourth
  consumer (`float_moment_hip`).
- [ADR-0266](../../adr/0266-hip-fifth-consumer-float-ansnr.md) — fifth consumer
  (`float_ansnr_hip`), retained for historical traceability. The kernel and its
  CPU twin were removed in
  [ADR-0709](../../adr/0709-vmafx-phase4b-distributed-platform.md) (PR #38);
  ANSNR is no longer a registered feature on any backend.
- [ADR-0267](../../adr/0267-hip-sixth-consumer-motion-v2.md) — sixth consumer
  (`motion_v2_hip`).
- [ADR-0372](../../adr/0372-hip-batch1-integer-psnr-float-ansnr.md) — batch-1
  kernels.
- [ADR-0373](../../adr/0373-hip-batch2-float-motion.md) — batch-2 kernels.
- [ADR-0375](../../adr/0375-hip-batch3-float-moment-float-ssim.md) — batch-3
  kernels.
- [ADR-0377](../../adr/0377-hip-batch4-ciede-motion-v2.md) — batch-4 kernels.
- `docs/adr/0379-hip-float-vif.md` — unavailable historical reference for
  `float_vif_hip`; [ADR-0592](../../adr/0592-hip-float-vif-stub-removal.md)
  records the later removal of its weak stub after the real kernel shipped.
- [ADR-0380](../../adr/0380-ffmpeg-patches-hip-backend-selector.md) — FFmpeg
  selector.
- [ADR-0468](../../adr/0468-hip-float-adm-real-kernel.md) — `float_adm_hip`.
- [ADR-0523](../../adr/0523-hip-integer-motion-extractor-registration.md) —
  register `vmaf_fex_integer_motion_hip`.
- [ADR-0533](../../adr/0533-hip-all-extractors-registration-sweep.md) — full
  HIP-extractor registration sweep (six more TUs wired into `hip_sources` and
  `feature_extractor_list[]`).
- [Research-0432](../../research/0432-hip-applicability.md) — AMD market-share
  and ROCm Linux maturity survey.

## Former section names

ADRs and research digests link to these headings; each points to the section
that now holds its content.

### A frame clears its accumulators after its upload (ADR-1427)

Now under [clearing accumulators after the
upload](uploads.md#clearing-accumulators-after-the-upload).

### `vif_hip` returns the CPU's scores bit for bit (2026-10-01)

Now under [vif_hip in the twin notes](twins.md#vif_hip).
