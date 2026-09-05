<!-- markdownlint-disable MD060 -->
# SYCL Backend

The SYCL / oneAPI backend runs VMAF's core feature extractors (VIF, ADM,
Motion) on any SYCL-capable accelerator. It is the fork's primary path
for Intel GPUs (Arc, integrated UHD / Iris Xe, Data Center GPU Flex/Max)
and also targets AMD via the HIP plugin and NVIDIA via the CUDA plugin
when the DPC++ compiler is built with those backends.

## Build

```bash
meson setup build -Denable_sycl=true
ninja -C build
```

Requires Intel oneAPI DPC++ (`icpx`). A bundled self-contained deployment —
useful for shipping the binary to hosts that don't have oneAPI installed —
is described in [bundling.md](bundling.md).

Meson options:

- `-Denable_sycl=true` — compile the SYCL backend + kernels.
- `-Denable_cuda=true` can be set in parallel; both backends can coexist
  in a single binary.

### `icpx` const-correctness on string default options

Per-extractor `VmafOption` rows declare `default_val.s` as `char *`
(matching the C API contract in `core/include/libvmaf/libvmaf.h`).
The DPC++ compiler (`icpx`) is stricter than g++ about C++
const-correctness and rejects initializing a `char *` member from a
`const char *` source. SYCL feature kernels that need a string default
should use `static char NAME[] = "..."` (array decay) rather than
`static constexpr const char *NAME = "..."`. The CUDA twins use a
`#define NAME "..."` macro for the same reason. Applies to every
`*_sycl.cpp` extractor that declares a string-typed default option.

## Runtime

When built with SYCL, the backend is auto-selected on hosts that expose a
Level Zero device. CLI controls:

```bash
./build/tools/vmaf ...                   # SYCL used automatically
./build/tools/vmaf --no_sycl ...         # force CPU path
./build/tools/vmaf --sycl_device 1 ...   # pick device index 1 explicitly
```

Device index `0` (the default) is whichever device SYCL's default selector
picks — usually the first discrete GPU. Use `--sycl_device` to pin an
iGPU or a specific Arc card.

## Source layout

```text
core/src/sycl/                        # queue, USM, surface import
  common.{cpp,h}                         # SYCL queue + device selection
  picture_sycl.{cpp,h}                   # USM picture upload / CPU path
  dmabuf_import.{cpp,h}                  # Linux: zero-copy VA-API dmabuf import
  d3d11_import.cpp                       # Windows: D3D11 staging-texture import
core/src/feature/sycl/                # per-feature kernels
  integer_vif_sycl.cpp
  integer_adm_sycl.cpp
  float_adm_sycl.cpp                     # float ADM extractor (ADR-0202)
  integer_motion_sycl.cpp
```

## Design notes

- **Single-source DPC++.** Kernels are ordinary C++ lambdas submitted via
  `queue::parallel_for`. No GLSL shaders, no separate SPIR-V assets — device
  code is linked into the binary at build time by `clang-offload-wrapper`.
- **Unified Shared Memory (USM).** The backend uses `malloc_device` for
  per-feature scratch buffers and a shared allocation for pictures when
  zero-copy isn't available.
- **Zero-copy dmabuf import.** When the input is a VA-API surface (e.g. from
  a QSV-decoded FFmpeg frame), the backend imports the dmabuf directly via
  `ext::oneapi::experimental::external_memory` — no CPU upload. See
  [dmabuf_import.cpp](../../../core/src/sycl/dmabuf_import.cpp).
- **D3D11 staging-texture import (Windows).** The `vmaf_sycl_import_d3d11_surface`
  API accepts an `ID3D11Texture2D*` from a Windows decoder (MediaFoundation, DXVA2,
  Direct3D11 VideoProcessor). The implementation creates a staging texture with
  `D3D11_USAGE_STAGING + D3D11_CPU_ACCESS_READ`, calls `CopyResource` to pull the
  GPU surface into staging, `Map`s the staging tex for CPU read, and forwards the
  mapped pointer + row pitch into `vmaf_sycl_upload_plane`. This is **not zero-copy**
  — throughput is bounded by PCIe upstream (staging Map) + PCIe downstream
  (SYCL H2D). A zero-copy equivalent would need DXGI NT-handle sharing +
  DPC++ D3D11 interop,
  which isn't documented in oneAPI as of 2025.1. See [d3d11_import.cpp](../../../core/src/sycl/d3d11_import.cpp)
  and ADR-0103.
- **In-order queues per extractor.** Each feature extractor owns a SYCL
  in-order queue. The host-side dispatcher submits work without explicit
  event dependencies; dependencies within an extractor are handled by the
  in-order semantics.

## QSV / VA-API zero-copy: 10-bit correctness (ADR-1121)

The `libvmaf_sycl` FFmpeg filter runs VMAF directly on QSV-decoded VA-API
surfaces with no host round-trip. Two requirements must hold for it to produce
correct scores on a 10/12-bit pair — get either wrong and you get
`VMAF score: nan` with a wildly inflated `integer_motion`.

### Give each decoder its own QSV session (required)

If both decoders are created against the **same** `-hwaccel_device`, FFmpeg
shares one `AVHWFramesContext` → one `mfxSession` → one VA surface pool between
them. The two decoders then write into the **same physical `VASurfaceID`s**, so
the reference decoder overwrites the surface the distorted decoder just produced
(decode-order look-ahead). The filter reads reference content where it expects
distorted content. Symptom: a checksum/score pattern where
`sycl_dis[N] == sycl_ref[N±1]`.

The fix is a usage contract — libvmaf cannot see FFmpeg's session topology from
inside the filter, so it is **not** auto-detected. Give **each** input its own
QSV device:

```bash
ffmpeg \
  -init_hw_device drm=drm0:/dev/dri/renderD128 \
  -init_hw_device vaapi=va0@drm0 \
  -init_hw_device qsv=qsv_ref@va0 \
  -init_hw_device qsv=qsv_dis@va0 \
  -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qsv_ref -c:v hevc_qsv -i ref.mkv \
  -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qsv_dis -c:v av1_qsv  -i dis.mkv \
  -lavfi '[0:v][1:v]libvmaf_sycl=log_fmt=csv:log_path=out.csv' \
  -frames:v 500 -f null -
```

A single shared `qsv` device silently reintroduces the contamination.

### P010/P012 pixels are normalized in the import

VA-API stores 10-bit (P010) / 12-bit (P012) samples **MSB-aligned**
(`V_MSB = V_LSB << (16 − bpc)`), while the VMAF feature kernels expect
LSB-aligned `bpc`-bit integers. The CPU `libvmaf` path never sees this because
FFmpeg auto-converts `P010LE → YUV420P10LE` (a `>> 6` shift) to satisfy the
filter's pixel-format list. The SYCL import does the equivalent luma-only
`>> (16 − bpc)` shift — **fused into the Tile4 / Y-tiled de-tile kernel** on the
QSV hot path (no extra GPU pass), and a standalone `launch_p010_normalize()`
kernel on the rare LINEAR / readback fallbacks. It is a no-op for 8-bit NV12.
This is internal — no user action required — but explains why a hand-rolled VA
import that skips it sees `integer_motion` inflated by exactly `2^(16 − bpc)`
(64× at 10-bit).

## fp64-less device contract (T7-17)

All SYCL feature kernels in this fork are designed to run on devices that
**lack `sycl::aspect::fp64`** (Intel Arc A-series, most Intel iGPUs, many
mobile / embedded GPUs). No kernel emits double-precision floating-point
SPIR-V instructions, so the JIT does not need to fall back to int64
emulation, and there is no per-kernel performance penalty on fp64-less
devices.

Concretely:

- **ADM gain limiting** uses an int64 Q31 fixed-point split-multiply
  (`gain_limit_to_q31` in `integer_adm_sycl.cpp`). The CPU reference
  multiplies a 32-bit DWT coefficient by a `double` gain in `[1.0, 100.0]`;
  the device path replaces this with `gain_q31 = round(gain * 2^31)` and a
  16-bit-split int64 multiply, exact for the production gain values
  (`1.0`, `100.0`) and within ±1 LSB for fractional gains.
- **VIF gain limiting** runs entirely in fp32 (`sycl::fmin(g,
  vif_enhn_gain_limit)` over float operands). The host stores the gain as
  a `double` for parity with the CPU API; the launcher casts to `float`
  before kernel submission.
- **CIEDE / SSIM accumulators** avoid `sycl::reduction<double>`; partials
  are accumulated in 32-bit fixed point and reduced via
  `sycl::plus<int64_t>` over subgroups.

`VmafSyclState` records `has_fp64` at queue construction so future
fp64-only optimisations can branch on it; current kernels do not. The
init log line at `VMAF_LOG_LEVEL_INFO` confirms which path was taken — on
an fp64-less device it reads "device lacks native fp64 — kernels already
use fp32 + int64 paths, no emulation overhead". A previous WARNING-level
line ("using int64 emulation for gain limiting") was misleading: it
suggested an emulation-overhead fallback that never existed. See
[ADR-0220](../../adr/0220-sycl-fp64-fallback.md).

If you add a new SYCL kernel and it captures a `double` operand or calls
`sycl::reduction<double>`, the entire SPIR-V module is rejected by the
Level Zero runtime on Arc A-series — even if the offending kernel is
never submitted. Audit the lambda capture list and any `sycl::reduce*`
calls before merging.

## Picture pre-allocation

`vmaf_sycl_preallocate_pictures()` + `vmaf_sycl_picture_fetch()` back a
2-deep ring of USM-backed `VmafPicture` instances that callers hand to
`vmaf_read_pictures()`. Three modes:

| `pic_prealloc_method` | Backing | Use case |
| --- | --- | --- |
| `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_NONE` | No pool; `vmaf_sycl_picture_fetch` falls back to host `vmaf_picture_alloc` | CPU-fed pipelines, test harnesses |
| `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE` | `sycl::malloc_device` (GPU-resident) | Zero-copy decoder interop (decoder writes directly into device USM) |
| `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_HOST` | `sycl::malloc_host` (coherent, CPU-visible) | Decoders that must write from the CPU but want pool reuse |

The pool depth (2) matches the double-buffered shared-frame upload in
`VmafSyclState`, so frame N+1 can start filling slot 1 while frame N's
compute still consumes slot 0. The caller owns the ref returned by
`vmaf_sycl_picture_fetch` and must release it via `vmaf_picture_unref` when
done with it; the pool retains its own ref until `vmaf_close()`.

Minimal example:

```c
VmafSyclPictureConfiguration cfg = {
    .pic_params = { .w = 1920, .h = 1080, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P },
    .pic_prealloc_method = VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE,
};
vmaf_sycl_preallocate_pictures(vmaf, cfg);

for (unsigned i = 0; i < n_frames; i++) {
    VmafPicture ref, dis;
    vmaf_sycl_picture_fetch(vmaf, &ref);   /* device USM, caller writes */
    vmaf_sycl_picture_fetch(vmaf, &dis);
    /* ... fill ref.data[0] and dis.data[0] via decoder/upload ... */
    vmaf_read_pictures(vmaf, &ref, &dis, i);
}
vmaf_read_pictures(vmaf, NULL, NULL, 0);
```

See [ADR-0101](../../adr/0101-sycl-usm-picture-pool.md) for the design
rationale (Y-plane only, pool depth 2, refcount semantics).

## AOT targets (default, ADR-0568)

By default this fork compiles the SYCL backend with Intel's GPU ahead-of-time
(AOT) compilation instead of the portable SPIR-V JIT path. AOT embeds
native GPU ISA blobs for the listed Intel micro-architectures directly into the
binary so that no JIT compilation is needed at first launch.

### Why AOT by default

Without AOT, `icpx -fsycl` emits portable SPIR-V that is compiled to native
ISA by the Level Zero / IGC runtime on first use. That compilation typically
takes several seconds and is paid again after driver upgrades or binary
reinstallation. For short VMAF runs (a handful of frames) the JIT cost
dominates the total wall time. The HIP analogue (`hip_gfx_targets`) learned
the same lesson in PR #1329. Making AOT the default eliminates the silent
first-run penalty for every operator who builds with `-Denable_sycl=true`
without reading the documentation.

### Default target list

<!-- markdownlint-disable MD013 -->
The default `sycl_icpx_aot_targets` value covers the following Intel GPU
micro-architectures:

| Target | Silicon |
| --- | --- |
| `dg2-g10` | Arc A770 / A750 (DG2-G10) |
| `dg2-g11` | Arc A380 / Arc Pro A30M (DG2-G11) |
| `acm-g10` | Arc A770M / A730M (ACM-G10) — mobile |
| `acm-g11` | Arc A550M / A370M (ACM-G11) — mobile |
| `acm-g12` | Arc A350M (ACM-G12) — mobile thin |
| `tgllp` | Tiger Lake integrated (TGL-LP) |
| `adl-s` | Alder Lake-S integrated (desktop) |
| `adl-p` | Alder Lake-P integrated (mobile 28W) |
| `adl-n` | Alder Lake-N integrated (N-series) |
| `rpl-s` | Raptor Lake-S integrated (desktop) |
| `rpl-p` | Raptor Lake-P integrated (mobile) |
| `mtl-h` | Meteor Lake-H integrated (high-performance mobile) |
| `mtl-u` | Meteor Lake-U integrated (ultra-mobile) |
| `arl-h` | Arrow Lake-H integrated (high-performance mobile) |
| `arl-s` | Arrow Lake-S integrated (desktop) |
| `arl-u` | Arrow Lake-U integrated (ultra-mobile) |
| `lnl-m` | Lunar Lake-M integrated (requires icpx 2025.0+) |
| `bmg-g21` | Battlemage G21 dGPU (requires icpx 2025.1+) |
| `bmg-g31` | Battlemage G31 dGPU (requires icpx 2025.1+) |
<!-- markdownlint-enable MD013 -->

The fat binary also embeds a SPIR-V JIT fallback (`spir64`) for any device not
in the list, so an unlisted or future device still works — it just pays the
cold-start cost.

### Adjusting the target list

Override the default at configure time with `-Dsycl_icpx_aot_targets=`:

```bash
# Single-target fleet (Arc A380 only) — smallest binary:
meson setup build -Denable_sycl=true -Dsycl_icpx_aot_targets=dg2-g11

# JIT-only (SPIR-V, no AOT blobs) — smallest binary, first-run penalty:
meson setup build -Denable_sycl=true -Dsycl_icpx_aot_targets=''

# Dev machine with Arc A380 + Meteor Lake iGPU:
meson setup build -Denable_sycl=true -Dsycl_icpx_aot_targets='dg2-g11,mtl-h'
```

The option is ignored when `sycl_compiler != 'icpx'` (i.e. AdaptiveCpp builds
are unaffected).

### Known toolchain version constraints

- `lnl-m` (Lunar Lake): requires icpx 2025.0.0 or later. Older releases emit
  an "unknown device" warning and silently skip that target; the remaining
  targets and the SPIR-V fallback still work.
- `bmg-g21`, `bmg-g31` (Battlemage): requires icpx 2025.1.0 or later. Same
  graceful-skip behaviour on older toolchains.
- If your toolchain is older than 2025.0.0 and the build fails on an unknown
  target name, narrow the list to the targets your toolchain supports, or set
  `sycl_icpx_aot_targets=''` to disable AOT.

### AdaptiveCpp (acpp) side

The `sycl_acpp_targets` option currently defaults to `"generic"`, which is
AdaptiveCpp's portable SPIR-V / SSCP JIT path — the same cold-start trap as
the old icpx default. AOT under AdaptiveCpp requires `intel_gpu_<arch>` target
strings (supported in AdaptiveCpp 23.10+); that broadening is tracked as a
follow-up task (see Known gaps below).

## Backend dispatch knob

`VMAF_SYCL_DISPATCH` controls the SYCL graph-replay strategy:

| Value | Behaviour |
|---|---|
| `direct` | Submit kernels directly to an in-order queue (no graph). Lower per-frame overhead at small resolutions. |
| `graph` | SYCL graph replay (ADR-0483). Reduces kernel-launch overhead at ≥ 720p. |

When unset, an area-threshold heuristic selects `graph` above 1280 × 720 pixels
and `direct` below — **except the zero-copy / VA-import path (`libvmaf_sycl`),
which defaults to `direct`** regardless of resolution. The combined graph is a
net throughput loss there (its output is byte-identical to direct, but the
per-frame de-tile import plus the graph compute-barrier serialise
decode→compute, ~15–25 % slower at 4K — ADR-1121). To force the graph on the
zero-copy path anyway, set `VMAF_SYCL_USE_GRAPH=1` or
`VMAF_SYCL_DISPATCH=…:graph` (these override the default).
`VMAF_SYCL_USE_GRAPH` (boolean `true`/`false`) provides a simpler global override
without per-feature granularity.

`VMAF_SYCL_NO_GRAPH=1` is a **deprecated** alias for `VMAF_SYCL_USE_GRAPH=false`.
It still works but prints a deprecation warning to stderr and will be removed in
v4.0 (ADR-0841).

`VMAF_SYCL_IMPORT_DEBUG=1` logs, at `INFO`, the shared frame-buffer addresses at
init and, per import, the `VASurfaceID` / imported Level Zero pointer / target
buffer for ref and dis. Use it to diagnose VA surface-pool reuse or buffer
aliasing on the zero-copy path (see the separate-session requirement above and
ADR-1121). The variable is resolved once at init, so toggling it mid-run has no
effect.

See [ADR-0483](../../adr/0483-gpu-dispatch-parse-dedup.md) and the
[env-var reference](../../usage/env-vars.md#sycl-dispatch-knob).

## Profiling

- Intel VTune (`vtune-gui`) with the GPU Compute analysis type for kernel
  occupancy and EU utilization.
- `onetrace` from the [pti-gpu](https://github.com/intel/pti-gpu) project
  for Level Zero API-level tracing.
- For end-to-end wall-time comparisons against the CUDA / CPU paths, use
  `make test-netflix-golden` which records per-backend scores and timings.
- Programmatic profiling via `VmafSyclState.enable_profiling` — see
  [api/gpu.md](../../api/gpu.md#profiling-helpers) for the queue-event
  query API.

## Numerical tolerance vs the CPU scalar path

SYCL kernels target **close agreement** with the CPU fixed-point
path, not bit-exact equality. Like every GPU path for VMAF, different
reduction orders, parallel-prefix scans, and FMA contractions can
perturb the final accumulator by a fraction of a ULP. Agreement is
typically at ~6 decimal places of the pooled VMAF score.

The **Netflix golden-data gate is CPU-only** — see
[docs/principles.md §3.1](../../principles.md#31-netflix-golden-data-gate).
The SYCL backend's per-build numerics are pinned by fork-added snapshot
tests, not by the Netflix goldens.

Accelerator-dependent controls that reduce (but do not eliminate)
the deviation:

- **fp16 path is disabled for scoring.** Some Intel GPUs expose fp16
  arithmetic; libvmaf forces fp32 on the kernel so scores are portable
  across hosts with different fp16 rounding modes.
- **Work-group reductions use fixed iteration order** so the most
  common source of cross-run drift (non-deterministic reduction tree)
  is eliminated; the remaining deltas come from unavoidable arithmetic
  restructuring between scalar and parallel-prefix code.

## Known gaps

- **CAMBI** — SYCL twin (`cambi_sycl`) shipped in ADR-0371. Strategy II
  hybrid: three GPU kernels (spatial-mask, 2× decimate, 3-tap mode filter)
  and host CPU residual (`calculate_c_values` + top-K pooling). Matches the
  CPU scalar extractor at `places=4` on the synthetic
  `test_sycl_cambi_parity` fixture, but on real content it is **not**
  bit-exact: with the default model on the 576x324 `src01` pair the pooled
  `cambi` score differs by 2.7e-3 (7.2e-3 max per frame), tracked in
  [`docs/state.md`](../../state.md) as
  `T-SYCL-CAMBI-PARITY-DRIFT-2026-09-05`.
- **GPU twins are only reached through a model.** `--feature <name>`
  resolves via `vmaf_get_feature_extractor_by_name()`, a plain name match
  on the registry, so `--backend sycl --feature cambi` runs the CPU
  `cambi` extractor. A model's feature list resolves via
  `vmaf_get_feature_extractor_by_feature_name(name, flags)` and picks the
  `_sycl` twin (ADR-1100). To exercise a SYCL twin from the CLI, either
  pass a model that uses the feature (`--model version=vmaf_v1.0.16_3d0h`
  for `cambi_sycl` / `speed_chroma_sycl`) or name the twin explicitly
  through the C API (`vmaf_use_feature(vmaf, "cambi_sycl", NULL)`).
- **CIEDE2000** — no SYCL kernel; CPU fallback.
- **SSIM / MS-SSIM / PSNR / PSNR-HVS / ANSNR** — no SYCL kernels.
- **Float-twin extractors (`float_*`)** — the SYCL backend
  implements ANSNR / PSNR / Motion / VIF / ADM
  ([ADR-0202](../../adr/0202-float-adm-cuda-sycl.md)).
- **`float_motion` extra options (`motion_add_scale1`,
  `motion_add_uv`, `motion_filter_size`, `motion_max_val`,
  `motion3_score`)** — these CPU options came in via the upstream
  port from Netflix/vmaf
  [`b949cebf`](https://github.com/Netflix/vmaf/commit/b949cebf)
  (2026-04-29). As of T3-15(c) /
  [ADR-0219](../../adr/0219-motion3-gpu-coverage.md), the SYCL
  `integer_motion` extractor emits `motion3_score` in 3-frame
  window mode via host-side `motion_blend()` post-processing of
  `motion2_score`; the full options surface
  (`motion_blend_factor`, `motion_blend_offset`, `motion_fps_weight`,
  `motion_max_val`, `motion_moving_average`) is exposed.
  `motion_five_frame_window=true` is rejected with `-ENOTSUP` at
  `init()` (the 5-deep blur ring is still deferred). The
  `motion_add_uv=true` path is independent from motion3 and remains
  **not yet wired through to the SYCL backend** — UV-plane motion
  stays CPU-only. The SYCL `picture_copy()` callsites at
  [`src/feature/sycl/integer_ms_ssim_sycl.cpp`](../../../core/src/feature/sycl/integer_ms_ssim_sycl.cpp)
  and
  [`src/feature/sycl/integer_ssim_sycl.cpp`](../../../core/src/feature/sycl/integer_ssim_sycl.cpp)
  pass `0` for the new trailing `channel` argument (Y-plane only,
  preserving SYCL pre-port behaviour).
- **SSIMULACRA 2** — `ssimulacra2_sycl` shipped per
  [ADR-0206](../../adr/0206-ssimulacra2-cuda-sycl.md) (hybrid
  host/GPU pipeline, kernel lambdas held in IEEE-754 strict mode by
  the existing `-fp-model=precise`).
- **dmabuf import is Linux-only.** The VA-API → dmabuf fast path is
  gated on `#ifndef _WIN32` in `sycl/dmabuf_import.cpp`; on Windows,
  `vmaf_sycl_dmabuf_import` and `vmaf_sycl_import_va_surface` return
  `-ENOSYS` so the caller falls back to the D3D11 staging path
  (`d3d11_import.cpp`). DMA-BUF is a Linux kernel interface
  (`ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF`); Level Zero on Windows uses
  NT handles instead.
- **HIP / ROCm via SYCL** — requires building DPC++ with the HIP plugin;
  the shipped Intel oneAPI binaries only include the Level Zero +
  OpenCL CPU + CUDA plugins.
- **`close_fex_sycl` forward declaration (SY-2a).** Each `init_fex_sycl`
  in `core/src/feature/sycl/` calls `close_fex_sycl(fex)` from its
  USM-allocation error paths so that partial allocations and
  `feature_name_dict` are released on init failure. Because the
  definition of `close_fex_sycl` lives at the bottom of every
  translation unit (next to the extractor's `VmafFeatureExtractor`
  registration struct), each TU adds a `static int
  close_fex_sycl(VmafFeatureExtractor *fex);` forward declaration just
  before the corresponding `init_fex_sycl` to keep strict C++ modes
  (icpx, msvc, clang `-Werror=implicit-function-declaration`)
  happy. The same pattern applies to `close_chroma_sycl` /
  `close_temporal_sycl` in `speed_chroma_sycl.cpp` and
  `speed_temporal_sycl.cpp`.
- **`cambi_high_res_speedup` and fp64-free speed extractors (ADR-1179).**
  The SYCL CAMBI extractor supports `cambi_high_res_speedup` (`hrs`),
  ensuring feature dictionary key and numerical parity with CPU CAMBI
  when running default model `vmaf_v1.0.16_3d0h`. Sizing of CAMBI histogram
  buffers is bounded by `MAX(num_bins, v_band_size)`. `speed_chroma_sycl`
  and `speed_temporal_sycl` use single-precision `float` accumulators and
  work-group accessors exclusively, strictly adhering to ADR-0220 on
  hardware lacking native double-precision support (Intel Arc A-series).

See [metrics/features.md](../../metrics/features.md) for the
per-extractor coverage matrix and [api/gpu.md](../../api/gpu.md#sycl)
for the programmatic surface.

## References

- [SYCL 2020 Specification](https://registry.khronos.org/SYCL/specs/sycl-2020/html/sycl-2020.html)
- [Intel oneAPI DPC++ Compiler](https://github.com/intel/llvm)
- [Level Zero Specification](https://spec.oneapi.io/level-zero/latest/)
- [Intel oneAPI Programming Guide](https://www.intel.com/content/www/us/en/docs/oneapi/programming-guide/current/overview.html)

## `integer_adm_sycl` and the default model's ADM (2026-09-05)

The default model `vmaf_v1.0.16_3d0h` requests
`VMAF_integer_feature_adm3_score` with `adm_csf_mode=2`,
`adm_dlm_weight=0.7`, `adm_enhn_gain_limit=1.0`, `adm_min_val=0.5` and
`adm_noise_weight=0.02`, under the key
`integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`.

`integer_adm_sycl` now honours `adm_csf_mode` (all four CSF models) and
`adm_p_norm`, and its `VmafOption` table is an entry-for-entry mirror of the
CPU table, so the `adm2` and `integer_adm_scale*` keys it emits are identical
to the CPU twin's for any options dict.

**`adm3_score` / `aim_score` are not emitted by this twin.** The AIM contrast
measure needs a second device CM pass with the `decouple_a` / `decouple_r`
roles swapped (the CUDA twin's ADR-0746 kernels); the SYCL kernel set does not
have one. Both features are therefore left out of `provided_features[]`, and
`vmaf_get_feature_extractor_by_feature_name()` routes a request for either to
the CPU `integer_adm` twin through the ADR-0530 fallback — correct value,
correct key. So on `--backend sycl` the default model's ADM branch runs on the
CPU while VIF / motion / CAMBI run on the device. Tracked as
`T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05` in
[`state.md`](../../state.md).

Two CPU-parity corrections landed with the option work: `adm_min_val` no
longer clamps `adm2` (the CPU floors the adm3 expression only), and the
`numden_limit` precision floor scales with the full-frame area rather than the
scale-3 area.
