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
is described in [bundling.md](bundling.md). For a native Windows build with
MSVC and oneAPI, see [SYCL on Windows](windows.md).

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
./build/tools/vmaf --backend sycl ...    # exit 100 if SYCL cannot initialise
./build/tools/vmaf --no_sycl ...         # force CPU path
./build/tools/vmaf --sycl_device 1 ...   # pick device index 1 explicitly
```

Automatic selection falls back to the CPU when SYCL cannot initialise, for
example when a Level Zero or Unified Runtime library fails to load or a
container has no render node. The run still prints a score and exits 0; only
a stderr line such as `problem during vmaf_sycl_state_init, using CPU` shows
it. `--backend sycl` makes the failure explicit: the CLI exits with code
`100` instead (see
[explicit-backend semantics](../index.md#explicit-backend-semantics-backend-name)).

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
  -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qsv_dis -c:v av1_qsv -i dis.mkv \
  -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qsv_ref -c:v hevc_qsv -i ref.mkv \
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
done with it; the pool retains its own ref until `vmaf_close()` returns exactly
zero. A nonzero close status retains the pool with the teardown-only context.

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
cold-start cost. The build forwards `-device <list>` with
`-Xsycl-target-backend=spir64_gen`; the target qualifier is required because an
unqualified `-Xs` also reaches the portable `spir64` target and oneAPI reports
the device selector as unused.

### What the build needs and checks

AOT needs Intel's `ocloc` offline compiler on `PATH`. The Linux oneAPI compiler
does not ship it; install it with `scripts/ci/install-intel-ocloc.sh` (see
[the oneAPI install guide](../../development/oneapi-install.md#the-ocloc-offline-compiler))
or configure with `-Dsycl_icpx_aot_targets=''`. Without it, `meson setup` stops
with an error that says so.

Each SYCL source file is compiled to native code for every listed target at
compile time (`-fno-sycl-rdc`), and the images are zstd-compressed
(`--offload-compress`), so every binary that links a SYCL object carries them:
`libvmaf.so`, static consumers of `libvmaf.a`, and the test executables. After
linking `libvmaf.so` on Linux, the build runs `core/src/sycl/check_aot_image.py`,
which fails the build unless the library holds a native image for every listed
target. You can check a library yourself:

```bash
clang-offload-bundler --list --type=o --input=build/src/libvmaf.so
# sycl-spir64_gen   <- native images
# sycl-spir64       <- SPIR-V fallback
```

Before [ADR-1360](../../adr/1360-sycl-aot-compile-time-device-codegen.md) the
link silently dropped the native images and every build was SPIR-V only.

Windows MSVC builds work differently, because Meson links them with
`link.exe`, which cannot handle the device code: the TUs keep relocatable
device code, and one `icpx -fsycl -fsycl-link` step generates the native
images for all of them and the object that registers them
([ADR-1364](../../adr/1364-windows-sycl-msvc-device-link.md),
[SYCL on Windows](windows.md)). The image check above does not run there.

Measured with the default 19 targets on a 22-thread host at `-j6`: a clean
build takes 162 s instead of 82 s for SPIR-V only, and `libvmaf.so` is 7.7 MB
instead of 4.6 MB. On an Arc B580 the default model's first frame with a cold
compiler cache drops from 524 ms to 201 ms (UHD 770: 609 ms to 245 ms).
Per-frame speed and scores are unchanged: with a warm cache the SPIR-V build
starts as fast, and AOT and JIT scores are bit-identical.

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

The target names are resolved by `ocloc`, so what counts is the ocloc
(compute-runtime) release, not the icpx version. The release pinned as
`INTEL_NEO_VERSION` in `build-config.env` knows every default target.

- `lnl-m` (Lunar Lake) and `bmg-g21`, `bmg-g31` (Battlemage) are the targets
  an older ocloc is most likely not to know; `ocloc ids <target>` tells you.
- An ocloc that does not know a listed target fails the compile, and a target
  that goes missing from the images fails the build-time image check. Narrow
  the list to what your ocloc supports, or set `sycl_icpx_aot_targets=''` to
  disable AOT.

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
perturb the final accumulator by a fraction of a ULP. Measured on an Intel
Arc A380 (2026-09-27) with `vmaf_v0.6.1` on the Netflix `src01` pair, the
pooled VMAF score differs from the CPU by 2.48e-5 and the largest per-frame
difference is 5.92e-5.

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

- **CAMBI** — SYCL twin (`cambi_sycl`) shipped in ADR-0415 as a Strategy II
  hybrid (GPU spatial mask, decimate and mode filter; host c-values and top-K
  pooling with a device-to-host copy per scale). Since
  [ADR-1357](../../adr/1357-sycl-cambi-device-resident.md) it runs every
  stage on the device, reads the distorted plane from the shared frame upload
  and reads back one 88-byte block per frame; at 3840x2160 it takes 9.3 ms a
  frame on an Arc B580 (was 140) and 42 ms on a UHD 770 (was 944). Per-frame
  scores are bit-identical to `--backend cpu` whenever the CPU's own top-K
  double sum is exact, and otherwise differ by that sum's rounding (at most
  2.2e-15 over 50 frames of Big Buck Bunny 4K). See
  [the CAMBI metric page](../../metrics/cambi.md#sycl).

  The history below describes the hybrid twin. Until branch
  `fix/gpu-cambi-parity-drift` the twin drifted from the CPU
  extractor on real content by 2.7e-3 pooled (7.2e-3 max per frame) on the
  576x324 `src01` pair: its spatial-mask kernel clamped out-of-image
  neighbours where `cambi.c` zero-pads them, and its vertical `filter_mode`
  pass overwrote the two border rows `cambi.c` deliberately leaves
  unfiltered. Both are fixed, and the measured parity is now:

  | Fixture | Frames | pooled `cambi_hrs_1080_cmxv_17_vlt_0.06` | max per-frame CPU↔SYCL delta |
  | --- | --- | --- | --- |
  | `src01` 576x324 | 48 | 0.2596781483085728 (CPU and SYCL) | 0 |
  | Tennis 1920x1080 | 10 | 0.5670459080762581 (CPU and SYCL) | 0 |
  | checkerboard 1px / 10px 1920x1080 | 3 each | 0 (CPU and SYCL) | 0 |

  Measured at `--precision max` (`%.17g`) on an Intel Arc A380, before
  ADR-1357. Every CAMBI GPU stage was integer-only and the c-value / pooling
  residual was the CPU code called through `cambi_internal.h`, which is why
  the emitted score agreed to every printed digit — this is a measurement
  on these four fixtures, **not** a general bit-exactness guarantee for the
  SYCL backend (see [ADR-0214](../../adr/0214-gpu-parity-ci-gate.md) for the tolerance
  contract). In that CAMBI measurement, pooled `vmaf` still differed by
  2.2e-6 on `src01` because the ADM / VIF / motion twins carry their own
  deltas; for the `vmaf_v0.6.1` figure see
  [Numerical tolerance vs the CPU scalar path](#numerical-tolerance-vs-the-cpu-scalar-path).
- **GPU twins are only reached through a model.** `--feature <name>`
  resolves via `vmaf_get_feature_extractor_by_name()`, a plain name match
  on the registry, so `--backend sycl --feature cambi` runs the CPU
  `cambi` extractor. A model's feature list resolves via
  `vmaf_get_feature_extractor_by_feature_name(name, flags)` and picks the
  `_sycl` twin (ADR-1100). To exercise a SYCL twin from the CLI, either
  pass a model that uses the feature (`--model version=vmaf_v1.0.16_3d0h`
  for `cambi_sycl` / `speed_chroma_sycl`) or name the twin explicitly
  through the C API (`vmaf_use_feature(vmaf, "cambi_sycl", NULL)`).
- **CIEDE2000** — `ciede_sycl`. The luma and both chroma planes are
  uploaded at their native size, and the kernel reads chroma at the
  subsampled position (the CUDA and HIP twins index the same way). The
  host no longer upscales chroma to full resolution before the copy,
  which roughly halves the per-frame time at 4K on an Arc B580. Scores
  are unchanged; they sit within 1e-4 of the CPU `ciede` (the parity gate
  allows 5e-3).
- **SSIM / MS-SSIM / PSNR / PSNR-HVS** — SYCL twins exist
  (`integer_ssim_sycl`, `float_ssim_sycl`, `float_ms_ssim_sycl`,
  `psnr_sycl`, `psnr_hvs_sycl`); `float_ansnr` was removed per
  [ADR-0865](../../adr/0865-ansnr-sunset-pre-vmaf-metric-drop.md).
- **Float-twin extractors (`float_*`)** — the SYCL backend
  implements PSNR / Motion / VIF / ADM
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
  The `motion_max_val` clipping bound is strictly honored on both
  `integer_motion2` and `integer_motion3` scores, matching the CPU
  reference semantics and eliminating drift on high-motion content
  (such as 1080p checkerboard pairs).
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
  the existing `-fp-model=precise`). The Charalampidis recursive blur is
  pure float32 matching the CUDA twin; pseudo-Kahan recurrence attempts are
  strictly forbidden as the 3-pole IIR filter has no running accumulator and
  diverges exponentially when perturbed. Under Arc A-series (e.g. A380) and
  other fp64-less hardware, cross-backend differences accumulate across the
  6-scale pyramid, leading to ~1.2e-2 delta on natural video sequences.
  This divergence is calibrated via `scripts/ci/gpu_ulp_calibration.yaml`
  at places=1 (`5.0e-2`) per
  [ADR-0985](../../adr/0985-sycl-parity-divergence-2026-06-03.md).
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
  and `speed_temporal_sycl` use single-precision `float` arithmetic and
  work-group accessors exclusively, strictly adhering to ADR-0220 on
  hardware lacking native double-precision support (Intel Arc A-series).
- **SpEED twins are device-resident and bit-identical to the CPU
  ([ADR-1358](../../adr/1358-sycl-speed-device-resident-linalg.md)).**
  `speed_chroma_sycl` and `speed_temporal_sycl` upload the raw planes once
  per frame and run filtering, covariance, eigenvalues, QR solve and score on
  the device, replayed as one recorded SYCL graph; the host reads one result
  per frame. Their per-frame scores equal `--backend cpu` exactly. Timings and
  the parity check are in [SpEED](../../metrics/speed_qa.md#sycl-device-resident-and-bit-identical-to-the-cpu).
  The four SpEED TUs build with `-ffp-contract=off`: `-fp-model=precise`
  alone still contracts `a * b + c` into an FMA inside kernels and leaves
  fp32 division and square root non-correctly-rounded (measured with icpx
  2026.1), so the pipeline rounds those two explicitly.

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

Two CPU-parity corrections landed with the option work: `adm_min_val` no
longer clamps `adm2` (the CPU floors the adm3 expression only), and the
`numden_limit` precision floor scales with the full-frame area rather than the
scale-3 area.

**`aim_score` and `adm3_score` come from the device (ADR-1362, 2026-09-29).**
Until this change the twin had no AIM pass and left both features to the CPU
`integer_adm` extractor, so under `--backend sycl` the default model scored
ADM on the CPU beside the device VIF, motion and CAMBI. The twin now runs the
CPU's second contrast-masking pass itself: the masking threshold comes from
the CSF-weighted restored signal and the measured signal is the additive
impairment, the two roles swapped relative to the DLM pass. It shares one
reduction kernel with the DLM measure and the CSF denominator, and the
accumulators still come back in one small copy per frame. To see the two
features from the twin itself, name it:
`vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 --backend sycl
--feature adm_sycl --no_prediction --json -o out.json` lists `integer_aim` and
`integer_adm3` next to `integer_adm2`.

How close the twin's ADM features are to `--backend cpu`, measured at
`--precision max` on an Arc B580 and a UHD 770 (Netflix `src01` pair, 50
frames of Big Buck Bunny 3840x2160, 853x480 and 17x17 crops, 10-bit input):

| Feature | Difference from the CPU extractor |
| --- | --- |
| `integer_aim`, `integer_adm3`, `integer_adm2`, `integer_adm_scale0..3` | none: bit-identical on every frame |
| any of them with a non-integer `adm_enhn_gain_limit` (e.g. 1.2) | up to 1.4e-6, from the fixed-point gain limit; the shipped models use 1.0 or 100 |

Before this change adm2 and the scale outputs were up to 2.9e-7 from the CPU
(the twin finalised its sums in double where the CPU uses float), and
`integer_adm_scale2` up to 1.40e-6 on 4K content.

Default model, milliseconds per frame, before this change → after (median of
seven runs at 576x324 and three at 3840x2160; `--threads 0` is the CLI
default, under which a CPU extractor runs on the main thread):

| Size | `--threads` | Arc B580 | UHD 770 | CPU backend |
| --- | --- | --- | --- | --- |
| 576x324 | 0 | 2.80 → 2.83 | 9.32 → 10.78 | — |
| 576x324 | 16 | 2.14 → 3.01 | 8.51 → 10.97 | 0.73 → 0.62 |
| 3840x2160 | 0 | 48.4 → 9.2 | 75.7 → 87.5 | — |
| 3840x2160 | 16 | 24.3 → 9.7 | 40.5 → 79.4 | 32.1 → 26.5 |

On the B580 a 4K frame is now 2.5 to 5 times faster and no longer depends on
`--threads`. On the UHD 770 the default model gets slower, most of all with
`--threads 16`: the CPU used to compute ADM for several frames in parallel
with the iGPU, and now the iGPU does that work as well. SYCL on the UHD 770
stays slower than the CPU backend either way. At 576x324 the B580 numbers are
within the run-to-run spread. The CPU backend runs the same code in both
builds; its spread is load from other jobs on the machine.

The twin also accepts the CPU's `adm_skip_aim` option (`aim` becomes 0 and the
AIM pass is skipped). The HIP twin still has no AIM pass
(`T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05` in
[`state.md`](../../state.md)).

## SpEED-chroma reports singularity separately from failure (ADR-1202, 2026-09-06)

The SYCL SpEED-chroma twin previously treated any non-zero return from its
linear-algebra helper as "singular covariance matrix" and imputed the `uv`
score from the other chroma channel. That rule is correct for the CPU
extractor, where a non-zero return does mean singular, but not here: this twin
handles singularity internally (warn, zero the solution, return 0) and uses
the return value for API errors only. A real device error was therefore fed
into the imputation, and with both chroma channels failing it averaged to
`0.0` and reported success.

Singularity now travels in its own `bool *singular_out` and hard errors
propagate, so a device failure inside SpEED-chroma fails the frame instead of
emitting a `0.0` score. The twin also adopts the CPU rule that a channel with
exactly one singular side (reference or distorted) scores 0 rather than an
inflated value. The launch-geometry half of ADR-1202 was CUDA-only — this
twin's solve launch was already correct.

## CAMBI reads its device buffers in one copy (2026-09-17)

`integer_cambi_sycl.cpp` enqueued one `q.memcpy` per row when reading the
decimated image and mask back for the CPU residual. Both sides are packed at
the same pitch, so the region was already contiguous and the loop bought
nothing: it is now a single copy per buffer.

The CUDA twin carried a worse version of the same defect — a *blocking* copy per
row, which cost 0.602 s of a 1.03 s run over 48 frames of 1080p. See
[the CUDA overview](../cuda/overview.md#cambi-reads-its-device-buffers-in-one-transfer-2026-09-17)
for the measurements. **If you add a GPU twin that reads a plane back, copy it
in one transfer.**

## Arc B580, small frames and device faults (2026-09-29)

Three fixes from one investigation on an Arc B580 (Xe2) and a UHD 770
(Xe-LP); the measurements are in
[Research-2123](../../research/2123-sycl-b580-psnr-hvs-and-tile-halo-faults.md).

- **`psnr_hvs_sycl` runs on Xe2.** It used to crash the process with
  SIGSEGV on the B580: the Intel GPU compiler (IGC 2.41.5) crashed while
  compiling the kernel at SIMD32. Its 8x8 DCT now runs in local memory instead
  of one work-item's private memory. Scores are bit-identical wherever the old
  kernel ran, and a 4K frame costs 130 ms instead of 208 ms on the UHD 770.
- **Small frames no longer lose the device.** Frames 64 rows high or less
  used to end in `UR_RESULT_ERROR_DEVICE_LOST` with `adm_sycl`, and so did
  small frames with `vif_sycl` and `motion_sycl` on the B580. The tiled kernels
  now keep their tile loads inside the plane; scores for other frames are
  unchanged.
- **`vif_sycl` needs frames of at least 16x16.** Each VIF scale halves the
  plane and reflects its filter taps once, which only stays inside a plane of
  at least 9, 10, 12 and 16 pixels for scales 0 to 3. When libvmaf picks the
  twin itself, from a model's VIF features, frames below 16 pixels in either
  dimension are computed by the CPU `vif` extractor, with the log line
  `feature extractor 'vif_sycl' cannot honour WxH; computing 'vif' on the CPU`,
  and the scores are the CPU's. (The default model cannot run below 17x17
  anyway: its ADM needs that on every backend.) Naming the twin directly, as in
  `--feature vif_sycl`, fails instead:
  ``vif_sycl requires width >= 16 and height >= 16 (got 8x8); the CPU extractor
  `vif` computes smaller frames``. The CUDA, HIP and Metal twins do not check
  this yet (`T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29`).
- **`vif_sycl` is correct for odd widths.** When a scale's width was odd
  (854x480, 1366x768 and 853x480 are common examples), scales 1 to 3 were read
  at the wrong row stride and drifted from the CPU by up to 1.2e-3, which moved
  the VMAF score as well. They now agree with the CPU within 1e-6 at those
  sizes.
- **A device fault fails the run.** When the device reports a fault, the
  graph extractors (`adm_sycl`, `vif_sycl`, `motion_sycl`, `psnr_sycl`,
  `float_moment_sycl`) and `psnr_hvs_sycl` return `-EIO` for the frame, and the
  CLI stops with an error after a line such as
  `libvmaf ERROR SYCL graph wait: level_zero backend failed with error: 20
  (UR_RESULT_ERROR_DEVICE_LOST)`. Before, the graph extractors emitted scores
  for the faulted frame from stale buffers (about 1.0 for every ADM scale);
  only a later frame's upload noticed the fault, so a one-frame run exited 0.

At 3840x2160, `psnr_hvs_sycl` differs from the CPU `psnr_hvs` by up to
8.4e-4 dB per frame; at 576x324 the difference is 8.4e-5. The CPU accumulates
all per-coefficient errors of a plane in one `float`, whose rounding error
grows with the frame size, and the twin sums per block. The cross-backend gate
therefore scales the `psnr_hvs` tolerance with the frame's block count: 5e-4
up to 576x324, 3.34e-3 at 3840x2160
([ADR-1361](../../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md),
[the gate guide](../../development/cross-backend-gate.md)).

## CPU options on the PSNR, SSIM and float-motion twins (2026-09-29)

Four SYCL twins now take the CPU extractor's full option table
([ADR-1365](../../adr/1365-sycl-twin-cpu-option-parity.md)). Before, a model
that set one of these options computed the feature on the CPU (ADR-1183), and
naming the twin with the option failed with `unknown option`.

| Twin | Options added | Agreement with `--backend cpu` |
|---|---|---|
| `psnr_sycl` | `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse` | bit-exact, `apsnr_*` included |
| `integer_ssim_sycl` | `enable_db`, `clip_db` | as the linear score (up to 1.5e-8), mapped through the dB slope |
| `float_ssim_sycl` | `enable_lcs`, `enable_db`, `clip_db` | `float_ssim_l/c/s` within 8.3e-7; dB as above |
| `float_motion_sycl` | `motion_max_val` (`mmxv`) | within 5.6e-6, as the default score; frames at the cap exact |

Measured on an Arc B580 and a UHD 770 over the Netflix 576x324 pair, 853x480
(4:4:4, 8- and 10-bit) and 576x324 10-bit, with every option alone and
combined. The dB form of SSIM magnifies a linear difference by
`4.34 / (1 - ssim)`, so the fp32 twins land within 3.4e-5 dB of the CPU on
that content, and further apart as the score nears 1.

Behaviour that changed with the options:

- **Identical frames.** Both SSIM twins now score identical windows exactly 1,
  as the CPU does, so `enable_db` reports `+inf` and `clip_db` the CPU's
  ceiling, instead of the finite dB value of an fp32 rounding residue. The
  default linear scores moved by at most 1.1e-8 (2.2e-8 on identical frames,
  which now score exactly 1), far inside the twins' parity tolerance.
- **`motion_force_zero` on `float_motion_sycl`** was declared but ignored: the
  twin emitted real scores. It now emits zeros, like the CPU.
- **The debug `motion` score of `float_motion_sycl`** now carries
  `motion_fps_weight`, as on the CPU; before it was emitted unweighted.

With `--backend sycl`, the CPU extractor names run on these twins with the
options set ([ADR-1359](../../adr/1359-cli-feature-backend-twin.md)); the twin
names work too:

```bash
vmaf ... --backend sycl --feature psnr=enable_mse=true:enable_apsnr=true
vmaf ... --backend sycl --feature float_ssim=enable_lcs=true:enable_db=true:clip_db=true
vmaf ... --backend sycl --feature float_motion_sycl=motion_max_val=4
```

The JSON `feature_backends` receipt lists `psnr_sycl`, `float_ssim_sycl` and
`float_motion_sycl` for these runs. `--feature float_motion` on SYCL reports
`motion` and `motion2`; `motion3` needs `--backend cpu`.

The CUDA, HIP and Metal twins still lack these options; see
`T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26` in
[`state.md`](../../state.md).

## `motion_sycl` matches the CPU `motion` exactly (2026-09-29)

`motion_sycl` now gives the same `integer_motion2` and `integer_motion3`
scores as `--backend cpu`, bit for bit
([ADR-1371](../../adr/1371-sycl-motion-diff-first-pipeline.md)). It used to
blur each frame and compare the blurred frames, while the CPU blurs the
difference of the two frames and rounds after each filter pass. The results
differ by rounding, which averages out over large frames but not small ones:

| Frame | Worst `integer_motion2` difference before | After |
|---|---|---|
| 17x17 | 2.0e-4 | 0 |
| 33x33 | 1.3e-4 | 0 |
| 64x64 | 4.9e-5 | 0 |
| Netflix 576x324 pair | 1.3e-5 | 0 |
| 3840x2160 | 5.6e-6 | 0 |

Measured on an Arc B580 and a UHD 770 with 8- and 10-bit input; the
default-model VMAF score at 4K moves by at most 4e-6. `motion_v2_sycl` already
matched the CPU and now shares the same kernel. The motion step reads two
frames instead of one blurred frame, which costs about 11% more device time
at 4K (0.54 instead of 0.48 ms per frame on the B580, 6.8 instead of 6.2 ms on
the UHD 770); a default-model run is unchanged within run-to-run noise. To
check a build:

```bash
for b in cpu sycl; do
  vmaf -r src01_hrc00_576x324.yuv -d src01_hrc01_576x324.yuv -w 576 -h 324 -p 420 -b 8 \
    --no_prediction --feature motion --backend "$b" --precision=max --json -q -o "motion_$b.json"
done
python3 -c "import json; a, b = (json.load(open(f'motion_{x}.json'))['frames'] for x in ('cpu', 'sycl')); print(max(abs(p['metrics']['integer_motion2'] - q['metrics']['integer_motion2']) for p, q in zip(a, b)))"
```

It prints `0.0`. The CUDA, HIP and Metal `motion` twins still compare blurred
frames (`T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29` and its HIP and Metal rows in
[`state.md`](../../state.md)).

With `motion_add_uv=true`, `motion_sycl` no longer waits for the U and V
upload inside `submit()`. It stages both planes in pinned host memory and
uploads them on the queue that runs the motion kernels, so the frame keeps a
single wait, in `collect()`. On a 4K clip the host spends 0.6 ms per frame on
it instead of 5.4 ms on the UHD 770 (0.55 instead of 0.89 ms on the B580),
and the scores are unchanged.

## Licensing of the SYCL kernels (ADR-1250)

As with the other backends, a SYCL kernel implementing an upstream Netflix
metric keeps that code's terms and copyright notice, while fork-original SYCL
code is EUPL-1.2. Four files in `core/src/sycl/` additionally carry an outside
contributor's work and stay on their current terms until that contributor
agrees to a change. See [ADR-1250](../../adr/1250-eupl-fork-relicense.md).
