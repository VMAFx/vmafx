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

A SYCL build is an icx / icpx build for its host code as well. Its CPU
extractors take their math functions (`log10`, `pow`, `powf`, ...) from
glibc's `libm`, not from Intel's `libimf`: the build passes
`-no-intel-lib=libimf` to every link
([ADR-1495](../../adr/1495-icx-system-libm.md),
[build flags](../../development/build-flags.md#icx-builds-use-glibcs-math-library)).
So the CPU half of a SYCL build returns a GCC build's scores, and a SYCL
twin that equals its own build's CPU extractor equals a GCC build's too. The
option applies to the host link only; the kernels and their device math
libraries do not change.

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

- **ADM gain limiting** uses 64-bit integer arithmetic
  (`adm_gain_limit_product()` in `core/src/feature/adm_gain_limit.h`). The
  CPU reference multiplies a 32-bit DWT coefficient by a `double` gain in
  `[1.0, 100.0]` and truncates the product toward zero; the device path
  returns that truncated double product exactly, from the gain's 53-bit
  significand and two 64-bit products, for the production values (`1.0`,
  `100.0`) and for non-integer gains alike
  ([ADR-1413](../../adr/1413-adm-gain-limit-truncated-double-product.md)).
  Until ADR-1413 this was a Q31 fixed-point product, which was up to one
  off per sample for a non-integer gain.
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

## Scratch memory on Intel GPUs (ADR-1395)

A SYCL kernel uses **scratch memory** when the Intel graphics compiler puts a
private array in memory or spills registers there. On an Arc A-series GPU
under the Linux **xe** kernel driver, kernels that do return wrong values:
measured on an Arc A380 with compute runtime 26.35.39758.10 and IGC 2.41.5,
where the same kernels were correct under i915. Scores from those kernels are
simply wrong, with no error. The fork's rule is that SYCL kernels use no
scratch memory ([ADR-1395](../../adr/1395-sycl-kernels-no-scratch.md)).
Since 2026-10-01 none does: the list of kernels that still did,
[`core/src/sycl/scratch_ratchet.txt`](../../../core/src/sycl/scratch_ratchet.txt),
is empty. `float_adm_sycl` was the last extractor on it; until then it
returned NaN on an Arc A380 under xe and the run stopped with `problem
reading pictures`.

**What you see.** At the first SYCL initialisation on each device, libvmaf runs
two small probe kernels, one with a private array and one that spills. When
either returns wrong values it logs a warning like:

```text
libvmaf WARNING SYCL: Intel(R) Arc(TM) A380 Graphics returns wrong values from kernels that use
scratch memory (private-array probe: 256 of 256 work-items wrong; register-spill probe: 256 of 256).
Seen on Arc A-series GPUs under the Linux xe kernel driver, not under i915. No libvmaf SYCL
extractor uses scratch memory, so its scores are not affected. See
docs/backends/sycl/overview.md (ADR-1395).
```

The device is still used, and every libvmaf extractor gives correct scores on
it. The warning stays because it describes the device: a kernel from outside
libvmaf, or one added without the audit below, would be affected. A build
older than 2026-10-01 names the extractors that still used scratch memory in
this message; on such a build compute those features with their CPU
extractors (`--feature adm` rather than `adm_sycl`, or `--no_sycl`), or run
the GPU under i915. The probes cost 3 to 5 ms with a
warm compute-runtime kernel cache and about 0.4 s on the first run;
`VMAF_SYCL_SCRATCH_SELFTEST=0` skips them.

**Checking a device.** `test_sycl_kernel_scratch` (in `--suite sycl`) builds
every kernel libvmaf registers for the default GPU and reports each one that
uses scratch memory:

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 \
  python3 scripts/ci/run_meson_test.py -- -C build -v test_sycl_kernel_scratch
```

It fails when a kernel uses scratch memory: the ratchet list that used to
exempt the known ones is empty and stays empty
(`test_sycl_kernel_source_contract.py` rejects a new entry without a device).
It skips without a GPU, so CI, which has none, does not run it. On an Arc A380
and on an Arc B580 and an Arc Pro B60 (Xe2, xe driver) it audits 127 kernels
and finds none. Both Xe2 cards return correct values from the two probes, so
they log no warning; until
2026-10-03 the term kernel of `float_adm_sycl` used 128 bytes of scratch
memory there ([ADR-1501](../../adr/1501-sycl-float-adm-terms-large-grf-xe2.md)).

**SIMD-32 VIF kernels.** Every Intel GPU supports SIMD-16 sub-groups, so
`vif_sycl` never picks its SIMD-32 kernels there by itself. They take the
256-entry register file to avoid spilling. To run them anyway, for example to
check them on a new driver, set `VMAF_SYCL_VIF_SUBGROUP_SIZE=32` (or `16`); a
size the device does not support is ignored with a warning. On an Arc A380 at
3840x2160 the SIMD-32 path gives bit-identical scores to SIMD-16, at 23.4
ms/frame against 23.2, and 67 ms/frame with `vif_fused=true` against 24.6.

**Writing a kernel.** Keep arrays that are indexed at run time in local memory
or registers, and keep the live values within 128 registers per hardware thread
at the kernel's SIMD width. A small private array indexed by a value known only
at run time (`pixel.original[band]`) is enough to need scratch memory: select
by value instead, as `fadm_load_cm_pixel()` in `float_adm_sycl.cpp` does.
When a kernel cannot stay within 128 registers, give it the 256-entry file
with `VmafSyclKernelShape<SG, 256>` from `core/src/feature/sycl/sycl_compat.h`,
as `integer_vif_sycl.cpp` does. Xe-LP integrated GPUs (`tgllp`, `adl-*`,
`rpl-*`) have no 256-entry file and spill at that size anyway; a kernel that
fits them only at the SIMD-8 the compiler picks there, while Xe2 accepts no
sub-group below 16, takes `VmafSyclKernelShape<0, 256>`: no required
sub-group size, the large file where the target has one. The term kernel of
`float_adm_sycl.cpp` does
([ADR-1501](../../adr/1501-sycl-float-adm-terms-large-grf-xe2.md)). Then run
`test_sycl_kernel_scratch` on an Intel GPU.

The build log of a default-list build (every target of
`sycl_icpx_aot_targets`) also names each spill per target, which shows a
spill on a GPU you do not have:

```text
[bmg-g21] warning: in kernel '...launch_terms...': compiled SIMD32 allocated 128 regs and spilled around 2
```

The log does not show private arrays in memory; only the device test does.

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

<!-- markdownlint-capture -->
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
<!-- markdownlint-restore -->

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
target. `ocloc` writes the images of one source file in one of two forms: for two
or more listed targets (even when some share a GPU IP version) a fat binary, an
`ar` archive with one member per target, and for exactly one target, as in
`-Dsycl_icpx_aot_targets=dg2-g11`, a bare native binary with no archive. The check
accepts both. It reads the GPU IP version of a bare binary from the `IntelGT`
product-config note in its `.note.intelgt.compat` section, which is the value
`ocloc ids <target>` prints for that target, and rejects a binary built for a
GPU IP version that no listed target uses. The test
`core/test/test_sycl_aot_image_check.py` covers both forms without a GPU or
oneAPI. You can check a library yourself:

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

### Sub-group sizes and the AOT targets (ADR-1468)

A kernel can require a sub-group size (how many work-items one hardware
thread runs as SIMD lanes). Not every GPU generation offers every size, and a
kernel that requires one a target lacks does not compile for it. Because all
kernels of a source file are compiled together, that one kernel fails the
file, and with the default target list the build:

```text
[lnl-m] error: in kernel '...': Kernel compiled with required subgroup size 8,
which is unsupported on this platform
```

What the targets of the default list accept, measured with `ocloc`:

| Targets | 8 | 16 | 32 |
|---|---|---|---|
| `tgllp`, `adl-*`, `rpl-*` (Xe-LP) | yes | yes | yes |
| `dg2-*`, `acm-*` (Arc A-series) | yes | yes | yes |
| `mtl-*`, `arl-*` (Xe-LPG) | yes | yes | yes |
| `lnl-m`, `bmg-g21`, `bmg-g31` (Xe2: Lunar Lake, Arc B-series) | no | yes | yes |

So a kernel of this backend requires 16 or 32, nothing else
([ADR-1468](../../adr/1468-sycl-sub-group-sizes-every-aot-target.md)).
`sycl_compat.h` enforces it at compile time: `VMAF_SYCL_REQD_SG_SIZE(8)` or
`VmafSyclKernelShape<8, ...>` is a compile error in every configuration,
including a build for a single device or with an empty target list.

Six kernels required 8 until 2026-10-02 (the row sums of `float_motion_sycl`,
`float_adm_sycl` and `float_vif_sycl`, the sum walk of `ssimulacra2_sycl`
and two test probes), and the default build, which the dev container image
uses, failed from the first of them on. They require 16 now. On an Arc A380
their scores are unchanged bit for bit, none uses scratch memory, and a
3840x2160 frame takes at most 2 % longer (`float_motion_sycl` 4.27 ms instead
of 4.18). Whether they are scratch-free and exact at 16 on Xe2 and on
integrated GPUs has not been measured
(`T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02` in
[`state.md`](../../state.md)); a build with an empty target list had the same
defect on an Xe2 device at run time, where the runtime compiler refuses the
kernel.

A build configured for one device does not compile for the others, so two
tests stand in for the default build:

```bash
# No compiler, no device (suite fast): every size the sources require is one
# every default target accepts.
python3 core/test/test_sycl_sub_group_size_contract.py

# Needs ocloc, no device (suite sycl-aot): compiles every SYCL source file of
# this build for all 19 default targets, whatever list the build was
# configured with. Minutes; VMAF_SYCL_AOT_JOBS sets the parallel compiles.
meson test -C build --suite sycl-aot
```

Run the second one before pushing a change to a SYCL kernel from a build that
is not configured with the default list.

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

The SYCL twins return the CPU extractors' bits. Of the 22 features of the
[cross-backend gate](../../development/cross-backend-gate.md), 21 are
declared exact twins and compared with tolerance 0
([list](../../development/cross-backend-exact-twins.md)); `ciede` is within
1.4e-11 of the CPU (its math functions). `speed_chroma` and `speed_temporal`
joined the exact twins with
[ADR-1477](../../adr/1477-speed-upstream-double-math.md): their logarithms
are the host's, so they no longer depend on the build's math library
([below](#exact-twins-declared-as-a-group-2026-10-02)). Measured on an Intel
Arc A380 (2026-10-02) with the default model, the VMAF score of every frame
equals `--backend cpu` on the Netflix `src01` pair (48 frames) and on 50
frames of BBB 3840x2160. On 2026-09-27 the pooled score of the Netflix pair
still differed by 2.48e-5.

The equality holds between the twin and the CPU extractor of one build, and,
since [ADR-1495](../../adr/1495-icx-system-libm.md), between the twin and a
GCC build's CPU extractor: a SYCL build links glibc's math library like the
GCC build (see [Build](#build)). Before it, the CPU extractors of a SYCL build
called Intel's `log10` and `pow` and could differ from a GCC build in the last
place.

### What the SYCL compile line guarantees

Under icpx every SYCL feature kernel is compiled with
`-fp-model=precise -ffp-contract=off -foffload-fp32-prec-div -foffload-fp32-prec-sqrt`
([ADR-1367](../../adr/1367-sycl-strict-fp-every-feature-tu.md)). Each fp32
`+`, `-`, `*`, `/` and `sqrt` in a kernel then rounds the way the CPU
reference build rounds it: IEEE-754 round to nearest, one rounding per
operation, `a * b + c` never fused into an FMA, subnormal results kept. This
holds for the native images built for the `sycl_icpx_aot_targets` devices and
for the SPIR-V image compiled at first launch on any other device, and
`test_sycl_fp_arith_contract` checks it on the device. A Windows MSVC build
generates every image, native and SPIR-V, in its one explicit device link
([ADR-1364](../../adr/1364-windows-sycl-msvc-device-link.md)), which gets the
same flags; that path has not been measured on a GPU yet. A kernel that needs
a fused multiply-add says so with `sycl::fma()`.

`-fp-model=precise` on its own did not give this: it left `a * b + c` fused
and `/` and `sqrt` approximate (29% and 8% of random fp32 operands differ
from the host), although earlier versions of this guide said the kernels ran
in IEEE-754 strict mode.

The line alone does not make scores bit-identical. What a twin has to handle
beyond it:

- **Transcendental functions.** `sycl::log2`, `exp`, `pow`, `cbrt`, `sin`
  and `atan2` are not correctly rounded on the device and are not the host's
  libm. `float_vif_sycl` evaluates the CPU's `log2` polynomial instead
  (ADR-1422), and `ciede_sycl` its own functions on pairs of `float` values
  (ADR-1436, [below](#ciede_sycl-follows-the-cpu-ciede-to-14e-11-2026-10-01)).
- **Summation order.** A work-group reduction adds in a tree, not in the
  CPU's sequential order. The twins add integers where the terms allow it
  (`float_psnr`, `float_moment` up to 2^53 units), reproduce the CPU's
  sequential `double` sum from integer increments where they do not
  (`ssimulacra2`, ADR-1446; `float_moment` past 2^53 units, ADR-1497),
  or read the terms back and add them on the host in the CPU's order
  (`ssim`, ADR-1443; `float_ssim`, ADR-1463; `float_ms_ssim`, ADR-1466).
- **fp64 on the CPU.** SYCL kernels are fp32-only
  ([ADR-0220](../../adr/0220-sycl-fp64-fallback.md)); where the CPU evaluates
  an expression in fp64, the twin carries it as an exact pair of floats
  (SpEED, `float_adm`) or computes the fp64 value in 64-bit integers
  (`ssim`, `float_ssim`, `float_ms_ssim`, `ssimulacra2`).
- **Content the gate's fixtures do not have.** The repository's 10-, 12- and
  16-bit clips are 8-bit content shifted left. `float_moment_sycl` and
  `float_psnr_sycl` matched them and differed on full-range noise until
  ADR-1449 and ADR-1450.

Before that work, measured with icpx 2026.1 on an Arc B580 and a UHD 770 on
2026-09-29 (both give the same
result except where noted in the ADR), maximum absolute difference against
`--backend cpu --precision max`: bit-identical for `motion_v2`, `float_psnr`,
`psnr`, `float_moment`, `speed_chroma` and `speed_temporal`; within 2.2e-15
for `cambi` and 6.7e-12 for `ssimulacra2` (bit-identical since ADR-1446);
the others between 1e-8 (`ssim`)
and 8.4e-4 dB (`psnr_hvs` at 3840x2160), each inside its
[cross-backend gate](../../development/cross-backend-gate.md) tolerance.
The exception is `float_ssim` forced to `scale=1` at 3840x2160, 8.8e-5 from
the CPU because of the formula difference above; with the default automatic
scale a 4K `float_ssim` runs on the CPU extractor. The per-twin table is in
[Research-1367](../../research/1367-sycl-strict-fp-every-feature-tu.md).

AdaptiveCpp builds accept neither `-fp-model` nor the precision flags: they
compile with contraction off, and division and square root keep the backend's
default precision.

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
  which roughly halves the per-frame time at 4K on an Arc B580. Since
  ADR-1436 the scores are within 1.4e-11 of the CPU `ciede` (the parity gate
  allows 1e-9); see
  [below](#ciede_sycl-follows-the-cpu-ciede-to-14e-11-2026-10-01).
- **SSIM / MS-SSIM / PSNR / PSNR-HVS** — SYCL twins exist
  (`integer_ssim_sycl`, `float_ssim_sycl`, `float_ms_ssim_sycl`,
  `psnr_sycl`, `psnr_hvs_sycl`); `float_ansnr` was removed per
  [ADR-0865](../../adr/0865-ansnr-sunset-pre-vmaf-metric-drop.md).
  `float_ssim_sycl` decimates on the device at every `float_ssim` scale
  ([below](#float_ssim-decimation-on-the-device-2026-09-29)).
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
  `motion_five_frame_window=true` runs on the device too, on `motion_sycl`
  and `motion_v2_sycl`, with the CPU's bits
  ([Motion, five-frame window](../../metrics/motion.md#five-frame-window),
  [ADR-1491](../../adr/1491-gpu-motion-five-frame-window.md)); `motion_sycl`
  copies one more luma plane per frame for it, and refuses the option
  together with `motion_add_uv` (`-ENOTSUP`: the CPU has no chroma mode to
  match). The
  `motion_add_uv=true` path is independent from motion3 and remains
  **not yet wired through to the SYCL backend** — UV-plane motion
  stays CPU-only. The SYCL `picture_copy()` callsites at
  [`src/feature/sycl/integer_ms_ssim_sycl.cpp`](../../../core/src/feature/sycl/integer_ms_ssim_sycl.cpp)
  and
  [`src/feature/sycl/integer_ssim_sycl.cpp`](../../../core/src/feature/sycl/integer_ssim_sycl.cpp)
  pass `0` for the new trailing `channel` argument (Y-plane only,
  preserving SYCL pre-port behaviour).
- **SSIMULACRA 2** — `ssimulacra2_sycl` shipped per
  [ADR-0206](../../adr/0206-ssimulacra2-cuda-sycl.md) and has been
  device-resident since
  [ADR-1363](../../adr/1363-sycl-ssimulacra2-msssim-device-resident.md):
  one upload of the raw planes, colour conversion, XYB, blurs, SSIM and
  edge sums and downsample on the device, one 864-byte readback per frame.
  Like every SYCL feature TU it builds with contraction off and correctly
  rounded division (ADR-1367), and divides through `div_rn()`, so everything
  up to the sums is
  bit-identical to the CPU. Since
  [ADR-1446](../../adr/1446-sycl-ssimulacra2-cpu-bits.md) the sums are too
  ([below](#ssimulacra2_sycl-matches-the-cpu-ssimulacra2-exactly-2026-10-02));
  before, pairs of floats in a fixed tree put the score within about 1e-11
  of `--backend cpu`. The Charalampidis
  recursive blur is pure float32; pseudo-Kahan recurrence attempts are
  strictly forbidden as the 3-pole IIR filter has no running accumulator and
  diverges exponentially when perturbed. The places=1 (`5.0e-2`) Arc A380
  calibration of
  [ADR-0985](../../adr/0985-sycl-parity-divergence-2026-06-03.md) predated
  the device-resident chain and is removed: on an A380 the twin now equals
  the CPU on every measured frame. See
  [SSIMULACRA 2](../../metrics/ssimulacra2.md#sycl-device-resident-one-readback-per-frame).
- **MS-SSIM waits once per frame (ADR-1363).** `float_ms_ssim_sycl`
  enqueues the pyramid and every scale's passes in `submit()`, each scale
  writing its partials to its own span, and `collect()` waits once and sums
  them. Since ADR-1414 the twin computes the CPU's arithmetic and its
  per-scale means equal the CPU's
  ([below](#float_ms_ssim_sycl-computes-the-cpus-arithmetic-2026-10-01)).
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
  per frame and run filtering, covariance, eigenvalues and the QR solve on
  the device, replayed as one recorded SYCL graph; the host reads one result
  block per frame (status words, eigenvalues, per-block variances) and forms
  the entropies and the score from it with `speed.c`'s own `log2()` calls
  ([ADR-1477](../../adr/1477-speed-upstream-double-math.md)). Their per-frame
  scores equal `--backend cpu` exactly, on an icx and on a GCC CPU. Timings and
  the parity check are in [SpEED](../../metrics/speed_qa.md#sycl-device-resident-and-bit-identical-to-the-cpu).
  The pipeline rounds division and square root explicitly (`div_rn()` /
  `sqrt_rn()`) and keeps every product that feeds an add in a named
  temporary. It was written before every SYCL feature TU got contraction off
  and correctly rounded division (ADR-1367) and stays exact whatever the
  compile line.

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

`psnr_hvs_sycl` returns the CPU `psnr_hvs` scores bit for bit, at every frame
size and at 8 to 12 bits
([ADR-1401](../../adr/1401-psnr-hvs-sycl-hip-exact-twins.md)). The CPU
accumulates all per-coefficient errors of a plane in one `float`, so its score
depends on the order of the additions; the kernel stores the 64 terms of every
block and the host adds them in the CPU's order. The CPU takes its masking
threshold as a `double` product and square root; the kernel, which has no fp64,
gets the same `float` from an integer square root of the exact product. The
cross-backend gate compares this twin with tolerance 0
([the gate guide](../../development/cross-backend-gate.md)). The price is a
readback of 256 bytes per block (65 MB per 3840x2160 frame) and a sequential
host sum; see
[the psnr_hvs page](../../metrics/psnr-hvs.md#agreement-with-the-cpu-extractor)
for the timings. Before, the twin summed per block and was up to 1.1e-2 dB
from the CPU at 3840x2160 (8.4e-5 at 576x324), under a tolerance that grew
with the frame size
([ADR-1361](../../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md)). 4:0:0
input is scored on luma only, as on the CPU.

## CPU options on the PSNR, SSIM and float-motion twins (2026-09-29)

Four SYCL twins now take the CPU extractor's full option table
([ADR-1365](../../adr/1365-sycl-twin-cpu-option-parity.md)). Before, a model
that set one of these options computed the feature on the CPU (ADR-1183), and
naming the twin with the option failed with `unknown option`.

| Twin | Options added | Agreement with `--backend cpu` |
|---|---|---|
| `psnr_sycl` | `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse` | bit-exact, `apsnr_*` included |
| `integer_ssim_sycl` | `enable_db`, `clip_db` | bit-exact since [ADR-1443](../../adr/1443-sycl-ssim-cpu-arithmetic.md) (then: as the linear score, up to 1.5e-8, mapped through the dB slope) |
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
`motion`, `motion2` and `motion3`; before 2026-10-03 it reported no `motion3`
([below](#float_motion_sycl-emits-motion3-2026-10-03)).

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

### `motion_sycl` emits the SAD score and honours `motion_force_zero` (2026-10-02)

Two outputs of `--backend sycl --feature motion` differed from the CPU's in
what they contain, not in a value:

- **`VMAF_integer_feature_motion_sad_score` was missing.** The CPU `motion`
  extractor writes the frame's SAD score on every frame (weighted by
  `motion_fps_weight`, capped at `motion_max_val`, 0 on frame 0) and derives
  `motion2` / `motion3` from it. `motion_sycl` computed the value and
  published it only as the `debug` score `integer_motion`. It now writes the
  SAD score on every frame.
- **`motion_force_zero=true` was ignored.** The twin handled the option in a
  function libvmaf never calls for a SYCL extractor, so the run returned the
  measured `integer_motion2_force_0` / `integer_motion3_force_0` where the
  CPU returns 0. It now returns 0 for every output on every frame and, like
  the CPU, computes no SAD: no device memory and no kernels for this
  extractor. The shipped model `model/other_models/vmaf_v0.6.1mfz.json` sets
  the option: on `--backend sycl` it scored the Netflix 576x324 pair 76.668
  where `--backend cpu` scores 72.321; both give 72.321 now.

Measured on an Arc A380 (xe) at `--precision max` against a GCC build of the
CPU extractor: every output of every frame identical on 116 frames (Netflix
576x324 at 8, 10 and 16 bits and as 10-bit 4:2:2, a 1080p checkerboard,
full-range noise at 8 and 16 bits, a bright 16-bit 1080p pair, 48 frames of
BBB 3840x2160) under seven option sets: default, `debug`,
`motion_force_zero` with and without `debug`, `motion_moving_average`,
weight 1.5 with blend 0.5 / offset 2 / cap 4, and `debug` with weight 0.3
and cap 0.5. The kernels are unchanged, so the frame time is too. To check a
build:

```bash
for b in cpu sycl; do
  vmaf -r src01_hrc00_576x324.yuv -d src01_hrc01_576x324.yuv -w 576 -h 324 -p 420 -b 8 \
    --no_prediction --feature motion --backend "$b" --precision=max --json -q -o "motion_$b.json"
done
python3 -c "import json; a, b = (json.load(open(f'motion_{x}.json'))['frames'] for x in ('cpu', 'sycl')); print(all(p['metrics'] == q['metrics'] for p, q in zip(a, b)))"
```

It prints `True`; before, the SYCL frames had no
`VMAF_integer_feature_motion_sad_score`. The parity gate's `motion` and
`motion_debug` cells compare the SAD score since this change
([cross-backend gate](../../development/cross-backend-gate.md)).
`motion_metal` still lacks the output
(`T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02` in
[`state.md`](../../state.md)).

## float_ssim decimation on the device (2026-09-29)

CPU `float_ssim` shrinks both pictures before it computes SSIM, by a factor
it picks from the short side: `max(1, round(min(w, h) / 256))`, so 1 below
384 px, 2 at 853x480, 4 at 1920x1080 and 8 at 3840x2160. `scale=N` (2 to 10)
forces the factor and `scale=1` turns it off. `float_ssim_sycl` used to
implement only factor 1, so at 1080p and 4K the CPU extractor ran instead
and `--backend sycl` printed
`float_ssim_sycl cannot run 3840x2160 8-bit pictures with these options`.

Since [ADR-1370](../../adr/1370-sycl-float-ssim-device-decimation.md) the
twin does the whole reduction on the GPU. It uploads the raw luma samples
(one byte each at 8 bits, instead of four after a host fp32 conversion),
averages each `N x N` block with the CPU's rounding, and runs SSIM on the
smaller planes. The reduced planes are identical to the CPU's bit for bit at
8, 10, 12 and 16 bits, for every factor and for odd sizes. The run needs no
option and prints no warning:

```bash
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 \
    --backend sycl --feature float_ssim --json -o out.json
# out.json: "feature_backends": [{"extractor": "float_ssim_sycl", "backend": "sycl"}]
```

`enable_lcs`, `enable_db` and `clip_db` work at every factor. The CPU
extractor still runs, with the usual warning, only when the reduced picture
is smaller than SSIM's 11x11 window (for example 100x100 with `scale=10`).

Agreement with `--backend cpu` (largest difference over all frames, the same
on an Arc B580 and a UHD 770):

| Input | Factor | `float_ssim` |
|---|---|---|
| Netflix 576x324, 48 frames | auto (1), 2, 3 | 3.0e-7, 6.0e-8, 6.0e-8 |
| BBB 1920x1080, 24 frames | auto (4), 3 | 4.3e-5, 3.7e-5 |
| BBB 3840x2160, 24 frames | auto (8) | 4.3e-5 |
| BBB 853x480 4:4:4, 24 frames | auto (2), 3 | 4.5e-5, 4.8e-5 |

What remains is the twin's fp32 SSIM arithmetic, which uses the combined
Wang formula rather than the CPU's luminance x contrast x structure product
([Research-0985](../../research/0985-sycl-parity-divergence-2026-06-03.md)).
The automatic factors stay inside the 5e-5 cross-backend tolerance; an
explicit `scale=1` or `scale=2` on BBB 1080p is 7.8e-5 or 5.6e-5 from the CPU,
as `scale=1` was before. The dB form magnifies these differences near 1.

Time per frame at 3840x2160, 8-bit, whole `vmaf` run, median of three:

| Configuration | ms / frame |
|---|---|
| CPU `float_ssim`, `--threads 16` (Core i9-12900K) | 11.0 |
| `float_ssim_sycl`, Arc B580 | 6.7 |
| `float_ssim_sycl`, UHD 770 | 12.0 |
| Before: `--backend sycl` fell back to the CPU, default threads | 29.3 |

The B580 figure is 0.6 ms above what the `vmaf` tool itself spends per 4K
frame. With `scale=1` the B580 drops from 10.6 to 7.8 ms because of the
smaller upload. [Research-2130](../../research/2130-sycl-float-ssim-device-decimation.md)
has the exactness argument and every measurement.

The CUDA, HIP and Metal `float_ssim` twins still compute factor 1 only and
fall back above it; their porting notes are in [`state.md`](../../state.md).

## psnr, psnr_hvs and motion_v2 share the uploaded frame (ADR-1369, 2026-09-29)

Every SYCL run uploads the luma of both pictures once per frame into the
state's shared frame. `psnr_sycl`, `psnr_hvs_sycl` and `motion_v2_sycl` now
read it there instead of uploading their own copies, and chroma goes up once
per frame into shared Cb / Cr planes that exist only when a twin that reads
chroma is in use — a luma-only run, such as the default model, never uploads
chroma. The chroma is packed into a pinned staging buffer and copied with one
DMA per plane. Nothing but the results comes back to the host.
[ADR-1369](../../adr/1369-sycl-shared-planes-light-twins.md) has the design and
[Research-1369](../../research/1369-sycl-shared-planes-light-twins.md) the
per-phase profile.

Milliseconds per frame at 3840x2160 (Big Buck Bunny, 8-bit 4:2:0), measured as
(t(22) - t(2)) / 20, median of 5, with the twin named explicitly
(`--backend sycl -n --feature psnr_hvs`):

| Twin | 16 CPU threads | Arc B580 before | Arc B580 after | UHD 770 before | UHD 770 after |
| --- | --- | --- | --- | --- | --- |
| `psnr_hvs` | 6.6 | 17.1 | 7.6 | 124.0 | 59.9 |
| `psnr` | 5.4 | 7.9 | 7.2 | 25.3 | 12.3 |
| `motion_v2` (against [ADR-1371](../../adr/1371-sycl-motion-diff-first-pipeline.md)) | 4.4 | 6.1 | 7.4 | 15.4 | 15.4 |

On the B580 `psnr` and `motion_v2` were already at the rate the CLI reads 4K
frames, and their 22-frame differences are within the measurement noise; over
100 frames `motion_v2` measures 6.1 ms before and 5.4 after (the host copy and
upload it no longer does). Its kernel is the ADR-1371 motion pipeline, which
this change does not touch. The default model
is unchanged (45.4 ms per 4K frame on the B580). At 576x324 every per-frame
difference is below the timer noise of the CLI measurement. Scores are
bit-identical to the previous twins on both devices, so `psnr` and `motion_v2`
still equal the CPU and `psnr_hvs` keeps its distance from the CPU inside the
[ADR-1361](../../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md) gate.

- **psnr_hvs at 9 and 11 bits** now scores the raw sample, as the CPU does;
  it used to score 16 times the sample (through the C API; the CLI accepts
  8, 10, 12 and 16 bits).
- **Zero-copy VA import** (FFmpeg `libvmaf_sycl` with QSV surfaces) imports
  luma only and hands the extractors no host pictures. `motion_v2_sycl` and
  `psnr_hvs_sycl` with `enable_chroma=false` no longer read host pictures, so
  they need only the imported luma; `psnr_sycl` and `psnr_hvs_sycl` with
  chroma fail the frame with `psnr_sycl: frame N chroma not on the device
  (-22)` or `psnr_hvs_sycl: frame N planes not on the device (-22)` where they
  used to dereference the missing picture. This path was not run for this
  change (no VA-API decode under WSL2).
- **One SYCL state, one frame size.** A state keeps the shared planes of the
  first frame size it sees. When a program reuses a state for a context with
  another size, twins that read chroma fail at init; luma-only twins score it
  wrong (`T-SYCL-SHARED-FRAME-STICKY-GEOMETRY-2026-09-29`). Create a state per
  geometry.
- **Profiling a twin.** `VMAF_SYCL_PROFILE=1` gives the primary and combined
  queues `enable_profiling`; with `VMAF_SYCL_NO_GRAPH=1` graph extractors
  submit directly, so every kernel has its own event. Research-1369 describes
  the event-timing build used for the numbers above.

## RC3 SYCL parity follow-ups (2026-09-30)

Three parity gaps between SYCL twins and the CPU reference were resolved:

- **SSIM flat/identical frames (`integer_ssim_sycl`, `float_ssim_sycl`).**
  Implemented CPU reference arithmetic matching the CUDA twin (#1637):
  `float_ssim_sycl` evaluates the CPU's per-pixel `l*c*s` with exact fp32 pairs
  (`Ff`) and fixed-point work-group sums, reduces in double on host, and
  preserves ADR-1370 fp32 frame-mean rounding. `integer_ssim_sycl` groups terms
  as `((weight * a) * b) / denominator` without identical-window shortcuts.
  Both match CPU behavior on flat 64x64 identical frames (72.247199 dB).
  (The pair terms and work-group sums of `float_ssim_sycl` were replaced by
  the CPU's doubles and a host sum in the CPU's order on 2026-10-02,
  ADR-1463.)
- **PSNR temporal subsampling (`integer_psnr_sycl`).** The twin carries
  the `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag, ensuring `--subsample` scaling
  operates consistently with temporal extractors.
- **Motion v2 weighting and clipping (`integer_motion_v2_sycl`).** `collect()`
  applies `motion_fps_weight` and caps at `motion_max_val`, matching CPU
  `integer_motion_v2.c::extract`. `flush()` derives `motion2_v2` and
  `motion3_v2` with CPU parity including one-frame inputs.

## `float_motion_sycl` matches the CPU `float_motion` exactly (2026-10-01)

`float_motion_sycl` returns the CPU extractor's `motion` and `motion2` bit for
bit ([ADR-1411](../../adr/1411-sycl-float-motion-cpu-float-sum.md)). It used
to sum the absolute differences of each 32x4 work-group on the device and the
groups in `double` on the host. The CPU extractor adds a row into one `float`,
the row sums into another, and divides in `float`; the result depends on that
order. The twin now adds each row on the device in the CPU's order, one
work-item per row, and the host adds the rows
(`core/src/feature/float_motion_sad.h`, shared with the CUDA twin).

Measured on an Arc A380 (xe driver, Level Zero, icpx 2026.0) at
`--precision max` against `--backend cpu`, frames identical and largest
difference before and after:

| Fixture | Before | After |
|---|---|---|
| Netflix 576x324, 48 frames | 1 of 48, 3.1e-6 | 48 of 48 |
| Checkerboard 1920x1080, 1 px shift, 3 frames | 1 of 3, 1.36e-4 | 3 of 3 |
| Checkerboard 1920x1080, 10 px shift, 3 frames | 1 of 3, 1.36e-4 | 3 of 3 |
| BBB 3840x2160, 200 frames | 1 of 50, 2.4e-5 (first 50) | 200 of 200 |

The one frame that matched before is frame 0, whose scores are 0. Also
identical after the change: the Netflix pair at 10, 12 and 16 bits and as
4:2:2 10-bit, and with `motion_fps_weight=0.5:motion_max_val=3`. The
reference was a GCC build of the CPU extractor; the CPU extractor of the icx
build gives the same values.

The row pass reads both blurred planes a second time. Through the `vmaf`
tool on the A380 that costs 0.38 ms per 3840x2160 frame (3.85 to 4.23 ms,
medians of 25 paired 200-frame runs; the untouched `float_psnr_sycl` read
3.37 and 3.38) and 0.07 ms per 576x324 frame (0.15 to 0.22). The kernel
requested a sub-group size of 8, the fastest of the exact shapes measured
(the ADR lists them); it requires 16 since 2026-10-02, because Xe2 devices
do not compile a kernel that requires 8
([below](#sub-group-sizes-and-the-aot-targets-adr-1468)).

The parity gate compares this twin with tolerance 0
([cross-backend gate](../../development/cross-backend-gate.md)). The twin
provides `motion3` since 2026-10-03 ([next section](#float_motion_sycl-emits-motion3-2026-10-03)).

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary build/tools/vmaf --features float_motion --backends cpu sycl \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324
```

The cell reports `exact:ADR-1397` and a largest difference of 0; it does so
on all four fixtures above.

## `float_motion_sycl` emits `motion3` (2026-10-03)

`float_motion_sycl` provided `motion` and `motion2` only. A
`--backend sycl --feature float_motion` run ran the twin and wrote no
`motion3` without a warning, and a request with `motion_blend_factor` or
`motion_blend_offset` ran on the CPU, because the twin did not declare them
(`T-GPU-FLOAT-MOTION3-MISSING-2026-09-30` in [`state.md`](../../state.md)).
The twin now provides `VMAF_feature_motion3_score` and takes both options
(aliases `mbf` / `mbo`), as the CUDA and HIP twins do. `motion3` is computed
on the host from the SADs the twin already returns bit for bit (ADR-1411),
with the CPU's `motion_blend_clip()`: the score is weighted by
`motion_fps_weight`, blended, then capped at `motion_max_val`. Frame 0 takes
the first SAD, the last frame comes from `flush()`, a one-frame input gets
`motion3` = 0, and `motion_force_zero` publishes 0 for it. No kernel changed.

Measured on the Arc A380 at `--precision max` against `--backend cpu`, all
outputs and pooled values identical:

| Fixture | Option sets | Frames identical |
|---|---|---|
| Netflix 576x324 | defaults, blend, `mfw=2:mmxv=4`, all four score options, `motion_force_zero` | 48 of 48, each of the three scores |
| Netflix 576x324, `--frame_cnt 1` | the same five | 1 of 1 (`motion3` = 0) |
| Checkerboard 1920x1080, 1 px and 10 px shift | the same five | 3 of 3 |
| BBB 3840x2160 | defaults, blend | 200 of 200 |

"Blend" is `motion_blend_factor=0.5:motion_blend_offset=2`, "all four" adds
`motion_fps_weight=2:motion_max_val=5` to `motion_blend_factor=0.25:motion_blend_offset=3`.
The parity gate's `float_motion` cell now compares `motion3` too; a twin that
lacks it fails the cell (`missing metrics`), which the twin before this change
does. `test_sycl_twin_option_parity` carries the `motion3`, blend,
weight-and-cap, `motion_force_zero` and one-frame cases, and also the cases
of the 2026-09-30 follow-ups above (flat identical frames, a single-pixel
frame, `apsnr` with `--subsample 2`, `motion_v2` weight, cap and one frame),
which it did not have before.

```bash
vmaf --reference ref.yuv --distorted dist.yuv --width 576 --height 324 \
    --pixel_format 420 --bitdepth 8 --backend sycl --no_prediction \
    --feature float_motion=motion_blend_factor=0.5:motion_blend_offset=2 \
    --precision max --json --output /dev/stdout
```

## `float_ms_ssim_sycl` computes the CPU's arithmetic (2026-10-01)

`float_ms_ssim_sycl` returns the CPU extractor's per-scale means bit for bit
([ADR-1414](../../adr/1414-sycl-float-ms-ssim-cpu-arithmetic.md)). Four things
differed from the CPU, the same four the CUDA twin had
([ADR-1403](../../adr/1403-cuda-strict-fp-every-kernel.md)):

- the decimate added `sample * tap` in two roundings where
  `ms_ssim_decimate.c` fuses each tap; it now calls `sycl::fma()`;
- the Gaussian window sums were fp32 running sums where `iqa_convolve()` adds
  fp32 products in `double`; they are exact pairs of floats now;
- `l`, `c` and `s` were fp32 quotients where the CPU divides `double`
  numerators by fp32 denominators for `l` and `c`; `l` and `c` are pairs now;
- the host combined unrounded means and took no `fabs()` of `l` and `c`; it
  now rounds each mean to fp32 and combines as `ms_ssim.c` does.

The pair arithmetic is the one `float_ssim_sycl` already used; both twins
take it from `core/src/feature/sycl/sycl_ssim_terms.h`. The per-pixel terms
are summed in 64-bit fixed point, which is exact in any order. No kernel uses
the fp64 type or scratch memory. (An exact sum is not the CPU's running sum;
since ADR-1466 the terms are the CPU's doubles and the host adds them in the
CPU's order,
[below](#float_ms_ssim_sycl-adds-its-per-scale-sums-in-the-cpus-order-2026-10-02).)

Measured on an Arc A380 at `--precision max` against a GCC build of the CPU
extractor: the score is identical on the Netflix 576x324 pair (48 frames;
before 6.9e-8), both 1080p checkerboard pairs (before 1.06e-6 and 2.98e-6)
and 199 of 200 frames of BBB 3840x2160 (before 1.23e-6), and every
`enable_lcs` and `enable_chroma` output is identical. The one remaining
frame differs by 1.1e-16 through the host's `pow()`: an icx build links
Intel's math library. The numbers per fixture are in
[MS-SSIM](../../metrics/ms-ssim.md#precision-of-the-gpu-twins).

The parity gate compares this twin with tolerance 0, with and without
`enable_lcs`. That cell runs the CPU extractor of the same binary, which on
an AVX-512 host must be built without FP contraction (#1706; before it, the
CPU `float_ms_ssim` of an icx build was one fp32 unit off in a per-scale mean
on 4 of 104 frames).

It costs time: 42.6 ms per 3840x2160 frame against 31.4 before, 1.20 ms per
576x324 frame against 0.84 (the CPU extractor: 117 ms at 3840x2160).

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary build/tools/vmaf --features float_ms_ssim float_ms_ssim_lcs \
    --backends cpu sycl \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324
```

## `float_vif_sycl` matches the CPU `float_vif` exactly (2026-10-01)

`float_vif_sycl` returns every output of the CPU extractor bit for bit
([ADR-1422](../../adr/1422-sycl-float-vif-cpu-arithmetic.md), after ADR-1412
for the CUDA twin). Four things changed:

- The Gaussian taps come from `vif_get_filter()`, which the CPU extractor
  calls at start-up. The twin held a table of decimals the CPU stopped using.
- `log2` is the CPU's polynomial `log2f_approx()`, not the device `log2`.
- The CPU evaluates `1 + (g * g * sigma1_sq) / (sv_sq + vif_sigma_nsq)` and
  `1 + sigma1_sq / vif_sigma_nsq` in `double`, because `vif_sigma_nsq` is one.
  A SYCL kernel has no `double`
  ([fp64-less contract](#fp64-less-device-contract-t7-17)), so the twin
  computes both as pairs of floats and, for a sample next to a rounding
  boundary (about one in 1650), replays the CPU's `double` operations in
  64-bit integers (`core/src/feature/sycl/sycl_float_vif_math.h`).
- The per-pixel terms are added as the CPU adds them: a row into one `float`,
  the rows into another. The twin used to reduce per sub-group and per 16x16
  block and add the blocks in `double`.

Measured on an Arc A380 (xe driver, Level Zero, icpx 2026.0) at
`--precision max` against `--backend cpu`, frames identical on all four
scales and largest difference before and after:

| Fixture | Before | After |
|---|---|---|
| Netflix 576x324, 48 frames | 0 of 48, 3.8e-5 | 48 of 48 |
| Checkerboard 1920x1080, 1 px shift, 3 frames | 0 of 3, 1.0e-6 | 3 of 3 |
| Checkerboard 1920x1080, 10 px shift, 3 frames | 1 of 3, 1.1e-12 | 3 of 3 |
| BBB 3840x2160, 200 frames | 0 of 20, 7.0e-6 (first 20) | 200 of 200 |

Also identical after the change: the Netflix pair at 10, 12 and 16 bits,
`debug=true` (15 outputs), and `vif_enhn_gain_limit`, `vif_sigma_nsq`,
`vif_skip_scale0` and the per-scale floors at non-default values. The
reference was a GCC build of the CPU extractor; the CPU extractor of the icx
build gives the same values.

The twin now runs three kernels per scale (filter, statistic, row sums)
instead of one. Through the `vmaf` tool on the A380 a 3840x2160 frame takes
23.95 ms, 20.54 ms before (medians of 15 paired 100-frame runs; the untouched
`float_psnr_sycl` read 3.34 and 3.33), and a 576x324 frame 0.93 ms, 0.73 ms
before. It uses 100 MB more device memory at 3840x2160. None of its kernels
uses [scratch memory](#scratch-memory-on-intel-gpus-adr-1395).

The twin accepts the CPU's `vif_scale1_min_val`, `vif_scale2_min_val` and
`vif_scale3_min_val` options now. The parity gate compares it with tolerance
0 ([cross-backend gate](../../development/cross-backend-gate.md)).

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py \
    --backend sycl --vmaf "$PWD/build/tools/vmaf" --feature float_vif
```

It prints, per output, how many frames are bit-identical and the largest
difference, and exits 0 only when every frame is.

## `vif_sycl` matches the CPU `vif` exactly (2026-10-01)

`vif_sycl` returns every output of the CPU integer VIF extractor bit for bit
([ADR-1432](../../adr/1432-sycl-integer-vif-exact-gain.md)). Two changes got
it there:

- The host stores each scale's numerator and denominator sum in a `float`,
  divides in single precision and adds the rounded sums for the debug
  outputs, as `integer_vif.c` does. It kept them in `double` before.
- `integer_vif.c` forms a pixel's gain in `double` and truncates
  `sigma2_sq - g * sigma12` and `g * g * sigma1_sq` to integers. The kernel
  used `float` for it ([fp64-less contract](#fp64-less-device-contract-t7-17)).
  It now gets both integers from one integer division by `sigma1_sq` and the
  remainder, and replays the CPU's `double` operations in 64-bit integers for
  a pixel whose value lies within the `double` chain's own rounding error of
  an integer: one pixel in 300 000 on real content
  (`core/src/feature/sycl/sycl_integer_vif_math.h`).

Measured on an Arc A380 (xe driver, Level Zero, icpx 2026.0) at
`--precision max` against `--backend cpu`, identical frames on the scale
that has the fewest and largest difference:

| Fixture | Before either change | Host sums only | Now |
|---|---|---|---|
| Netflix 576x324, 48 frames | 0 of 48, 3.5e-7 | 12 of 48, 3.6e-7 | 48 of 48 |
| Checkerboard 1920x1080, 1 px shift, 3 frames | 0 of 3, 3.9e-8 | 2 of 3, 6.0e-8 | 3 of 3 |
| Checkerboard 1920x1080, 10 px shift, 3 frames | 1 of 3, 1.5e-14 | 3 of 3 | 3 of 3 |
| BBB 3840x2160, 200 frames | 0 of 20 (first 20), 1.7e-7 | 140 of 200, 1.8e-7 | 200 of 200 |

Also identical now: the Netflix pair at 10, 12 and 16 bits and as 4:2:2
10-bit, `debug=true` (15 outputs), `vif_enhn_gain_limit` of 1.0, 1.2 and
37.5, `vif_skip_scale0`, and a 3840x2160 clip scored against itself. The
reference was a GCC build of the CPU extractor; the CPU extractor of the icx
build gives the same values.

Through the `vmaf` tool on the A380 a 3840x2160 frame takes 22.21 ms, 21.46
ms before (medians of 11 paired 100-frame runs), and a 576x324 frame 0.88 ms,
0.79 before. No kernel of the twin uses
[scratch memory](#scratch-memory-on-intel-gpus-adr-1395); the fused kernel of
scale 0 takes the large register file at SIMD-16 for it.

The twin's `debug` option defaults to `false`, the CPU's default. A run that
relied on the eleven debug outputs (`integer_vif`, `integer_vif_num`,
`integer_vif_den` and the per-scale sums) asks for them:
`--feature vif_sycl=debug=true`. The parity gate compares the twin with
tolerance 0 ([cross-backend gate](../../development/cross-backend-gate.md)).

`sycl::mul_hi()` on 64-bit operands returned wrong values in a kernel on the
A380; the twins form wide products in 32-bit limbs
(`core/src/feature/sycl/sycl_soft_double.h`).

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py \
    --backend sycl --vmaf "$PWD/build/tools/vmaf" --feature vif
```

## `ciede_sycl` follows the CPU `ciede` to 1.4e-11 (2026-10-01)

`ciede.c` computes in `double` and stores in `float`. A SYCL kernel has no
`double` ([fp64-less contract](#fp64-less-device-contract-t7-17)), and
`ciede_sycl` used `float` throughout, the device's `float` math functions and
a sum per 16x16 block. It was up to 1.14e-5 from the CPU. Since
[ADR-1436](../../adr/1436-sycl-ciede-cpu-arithmetic.md) the kernel runs the
CPU's statements with every `double` as a pair of `float` values (about 48
bits) and every math-library call as a function on such pairs
(`core/src/feature/ciede_ff_math.h` and `core/src/feature/ff_math.h`,
shared with the HIP twin since ADR-1448; `sycl_ciede_math.h` and
`sycl_ff_math.h` name the SYCL primitives they use), rounds to `float` where
the CPU does, and stores one `float` per pixel; the host adds them in the
CPU's order.

Each piece of the old twin on its own, largest difference on the Netflix
576x324 pair and on BBB 3840x2160
([Research-1436](../../research/1436-sycl-ciede-fp32-pairs.md) has all
thirteen):

| What the twin did | Netflix | BBB 4K |
|---|---:|---:|
| the linear Lab branch as `7.787 t + 16 / 116` (the CPU: `(24389 / 27 * t + 16) / 116`) | 1.12e-5 | 1.05e-6 |
| the CPU's constants as `float` | 5.3e-7 | 6.7e-7 |
| `x^2.4` from the device's `float` `pow` | 2.3e-7 | 5.9e-7 |
| the colour conversion arithmetic in `float` | 2.1e-7 | 2.1e-7 |
| sums of 256 pixels in `float`, those in `double` | 2.0e-7 | 8.0e-8 |
| the cube root from the device's `float` `cbrt` | 5.0e-8 | 3.8e-7 |

Measured on an Arc A380 (xe driver, Level Zero, icpx 2026.0) at
`--precision max` against `--backend cpu`, identical frames and largest
difference:

| Fixture | Before | Now |
|---|---|---|
| Netflix 576x324, 48 frames | 0 of 48, 1.14e-5 | 47 of 48, 6.9e-13 |
| Netflix 576x324 at 10, 12, 16 bits and 4:2:2 10-bit, 3 frames each | | 3 of 3 each |
| Checkerboard 1920x1080, 1 px and 10 px shift, 3 frames each | 3 of 3 | 3 of 3 |
| BBB 3840x2160, 200 frames | 0 of 20 (first 20), 1.3e-6 | 0 of 200, 1.4e-11 |

That is where the CUDA twin is, which has `double`. What is left is the C
library: on three 3840x2160 frames the device's 24.9 million values equal the
CPU's statements evaluated with a correctly rounded `powf` on every pixel;
18 to 64 per frame differ from the GCC build's CPU extractor, where glibc's
`powf` rounds the other way, and 7 to 30 from the icx build's. The twin is
therefore not listed as exact; the parity gate compares it at `1e-9`
([cross-backend gate](../../development/cross-backend-gate.md)).

Through the `vmaf` tool on the A380 a 3840x2160 frame takes 50.3 ms, 16.2 ms
before (medians of 11 paired runs, paired difference 33.9 ms), and a 576x324
frame 1.2 ms, 0.45 before. Of the 50 ms, 11 are the uploads, the read-back
and the host's sum, 19 the two Lab conversions and 21 the difference
formula. The twin keeps one `float` per pixel on the device
and in pinned host memory, 33 MB each at 3840x2160. No kernel uses
[scratch memory](#scratch-memory-on-intel-gpus-adr-1395): the per-pixel
function is flattened into the kernel, because a call inside a kernel takes
its frame from scratch memory.

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py \
    --backend sycl --vmaf "$PWD/build/tools/vmaf" --feature ciede --max-abs-diff 1e-9
```

## `integer_ssim_sycl` matches the CPU `ssim` exactly (2026-10-02)

`integer_ssim_sycl` returns the CPU fixed-point `ssim` extractor's score bit
for bit ([ADR-1443](../../adr/1443-sycl-ssim-cpu-arithmetic.md)). Its int64
window moments were already the CPU's. Two things were not:

- `integer_ssim.c` forms each pixel's term in `double`. The kernel used
  `float` for it ([fp64-less contract](#fp64-less-device-contract-t7-17)). It
  now runs the CPU's `double` operations, one for one and in the CPU's order,
  on a significand and an exponent held in 64-bit integers
  (`core/src/feature/sycl/sycl_integer_ssim_math.h` on
  `core/src/feature/sycl/sycl_soft_signed.h`), and stores the bit pattern of
  the resulting `double`.
- `calc_ssim()` adds every term into one `double`, left to right and top to
  bottom. The kernel added `float` partial sums per 16x8 work-group. The host
  now reads the plane of terms back and adds it in the CPU's order.

Measured on an Arc A380 (xe driver, Level Zero, icpx 2026.0) at
`--precision max` against `--backend cpu`:

| Fixture | Before | Now |
|---|---|---|
| Netflix 576x324, 48 frames | 0 of 48, 6.9e-9 | 48 of 48 |
| Checkerboard 1920x1080, 1 px shift, 3 frames | 0 of 3, 1.0e-7 | 3 of 3 |
| Checkerboard 1920x1080, 10 px shift, 3 frames | 0 of 3, 1.1e-7 | 3 of 3 |
| BBB 3840x2160, 200 frames | 0 of 200, 3.1e-7 | 200 of 200 |
| Netflix 576x324 at 10 and 12 bits and 4:2:2 10-bit, 3 frames each | 0 of 3, 4.4e-9 | 3 of 3 |
| Netflix 576x324 at 16 bits, 3 frames | the run failed (`invalid ratio`) | 3 of 3 |

What each cause contributed, from the new twin with one piece put back
(Netflix, 1 px checkerboard, 10 px checkerboard, BBB 20 frames):

| Piece put back | Largest difference |
|---|---|
| The term computed in `float` | 6.6e-9, 9.4e-8, 1.1e-7, 3.1e-7 |
| `float` partial sums per 16x8 block | 1.3e-8, 6.8e-8, 5.6e-8, 1.5e-8 |
| The exact term stored as a `float` | 9.4e-11, 3.2e-9, 3.3e-9, 3.7e-10 |
| `double` sums per 16x8 block (the order alone) | 2.3e-14, 1.6e-12, 1.1e-11, 5.6e-13 |

The reference was a GCC build of the CPU extractor; the CPU extractor of the
icx build gives the same scores. With `enable_db` the twin equals the CPU
extractor of its own build on every frame; against the GCC build 10 of 266
frames differ by at most 3.6e-15, which is the host's `log10` (Intel's math
library in an icx build, glibc's in a GCC build) and not the twin.

Through the `vmaf` tool on the A380 a 3840x2160 frame takes 31.9 ms, 17.8 ms
before (medians of 11 runs of 50 frames, host load average 9 to 11; the
`float_psnr` control read 3.28 ms on both builds), and a 576x324 frame 0.78
ms, 0.45 before. Of the 31.9 ms, 7.2 are the term's integer arithmetic, 5.8
the read-back of 66 MB, 2.8 the host's 8.3 million additions and 16.4 the
uploads and the two moment passes the twin had before. The CPU extractor
takes 111 ms. The term kernel runs at SIMD-16 with the large register file
and uses no [scratch memory](#scratch-memory-on-intel-gpus-adr-1395). The
window weight is no longer a device plane, so device memory is unchanged; the
twin holds 66 MB more pinned host memory at 3840x2160. Getting the time back
is `T-SYCL-SSIM-EXACT-THROUGHPUT-2026-10-02` in
[`state.md`](../../state.md).

The parity gate compares the twin with tolerance 0
([cross-backend gate](../../development/cross-backend-gate.md)).

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary build/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --features ssim --backends cpu sycl
```

## `float_adm_sycl` matches the CPU `float_adm` exactly (2026-10-02)

`float_adm_sycl` returns every output of the CPU float ADM extractor bit for
bit ([ADR-1434](../../adr/1434-sycl-float-adm-cpu-arithmetic.md), after
ADR-1420 for the CUDA twin). The DWT and the decouple's quotient were already
the CPU's (the CPU divides since
[ADR-1442](../../adr/1442-float-adm-reference-divides.md)); the rest was not.
What changed, with the largest difference each item alone leaves on the
Netflix 576x324 pair and on BBB 3840x2160
([Research-1434](../../research/1434-sycl-float-adm-fp64-free-arithmetic.md)):

| What the twin did | What `adm_tools.c` does | Netflix | BBB 4K |
|---|---|---:|---:|
| compared the angle test with `cos^2 * (o^2 * t^2)` | `(cos^2 * o^2) * t^2` | 2.4e-6 | 1.28e-5 |
| added the row sums in `double` on the host, per sub-group before | one `float` per row, then one over the rows | 1.9e-7 | 1.8e-7 |
| used `float` constants for 1/30 and 1/15 | `double` literals | 1.2e-9 | 2.4e-9 |
| added the centre tap of the masking threshold last | fifth of nine, per band | 1.2e-9 | 2.4e-9 |
| multiplied the enhancement gain in `float` (at `adm_enhn_gain_limit=1.2`) | in `double` | 0 | 6.0e-10 |

A SYCL kernel has no `double`
([fp64-less contract](#fp64-less-device-contract-t7-17)). The three `double`
expressions are evaluated as exact pairs of `float` values; a result that
lies next to a `float` rounding boundary, about one evaluation in 131 000 by
the width of the test, replays the CPU's `double` operations in 64-bit
integers (`core/src/feature/sycl/sycl_float_adm_math.h`). The CSF weights,
the reduced region, the pooling and the frame floor are the CPU's own
routines.

Measured on an Arc A380 (xe driver, Level Zero, icpx 2026.0) at
`--precision max` against `--backend cpu`, outputs identical and the largest
difference:

| Fixture | Before | Now |
|---|---|---|
| Netflix 576x324, 48 frames, 7 outputs | 59 of 336, 2.5e-6 | 336 of 336 |
| Checkerboard 1920x1080, 1 px shift, 3 frames | 1 of 21, 3.5e-7 | 21 of 21 |
| Checkerboard 1920x1080, 10 px shift, 3 frames | 4 of 21, 1.5e-7 | 21 of 21 |
| BBB 3840x2160, 200 frames | 301 of 1400, 1.7e-5 | 1400 of 1400 |

Also identical: the Netflix pair at 10, 12 and 16 bits and as 4:2:2 10-bit,
`debug=true` (18 outputs), `adm_enhn_gain_limit` of 1.0, 1.2 and 37.5,
`adm_bypass_cm`, `adm_noise_weight=0`, `adm_p_norm=1`, and the four options
the twin did not have before: `adm_skip_scale0`, `adm_skip_aim_scale`,
`adm_f1s0..3` and `adm_f2s0..3`. `adm_p_norm` other than 1 or 3 is within
1.8e-7 (the device's `pow` against the host's).

The reference in that table is the CPU extractor of the same build, which is
what the parity gate compares. Against a GCC build of the CPU extractor,
`aim` and `adm3` differ on 2 of the 200 BBB frames by 1.6e-9: the final roots
are the host math library's `powf`, Intel's in one build and glibc's in the
other. That difference is between the two CPU extractors.

Through the `vmaf` tool on the A380 a 3840x2160 frame takes 12.3 ms, 15.1 ms
before (medians of 11 runs of 50 frames), and a 576x324 frame 0.59 ms, 0.57
before. The twin stores nine terms per sample of the reduced region, 48 MB
of device memory at 3840x2160, and copies one block of row sums to the host
per frame, four before. No kernel uses
[scratch memory](#scratch-memory-on-intel-gpus-adr-1395).

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py \
    --backend sycl --vmaf "$PWD/build/tools/vmaf" --feature float_adm
```

## `ssimulacra2_sycl` matches the CPU `ssimulacra2` exactly (2026-10-02)

`ssimulacra2_sycl` returns the CPU extractor's score bit for bit
([ADR-1446](../../adr/1446-sycl-ssimulacra2-cpu-bits.md), after ADR-1433 for
the CUDA twin and ADR-1445 for the HIP twin). Its fp32 planes were already
the CPU's. Two things in the last stage were not:

- `ssimulacra2.c` forms six terms per sample and channel in `double`. The
  kernel formed each as a pair of `float` values, which is within about 2^-44
  of the `double` and not equal to it
  ([fp64-less contract](#fp64-less-device-contract-t7-17)). It now runs the
  CPU's `double` operations, one for one, on a significand and an exponent
  held in 64-bit integers (`core/src/feature/sycl/sycl_ssimulacra2_math.h` on
  `core/src/feature/sycl/sycl_soft_signed.h`).
- The CPU adds each term into one `double`, pixel after pixel. The kernel
  added the pairs in a fixed tree. It now forms the CPU's sums from parallel
  pieces as the CUDA twin does (`core/src/feature/ordered_sum.h`, used here
  through bit patterns in `core/src/feature/sycl/sycl_ordered_sum.h`): per
  chunk of 512 consecutive pixels the terms become whole-number steps of the
  running sum, composed in pixel order, and one walk per sum adds the chunks,
  term by term where the sum passes a power of two. The old pair sums remain
  as advice for that walk's plan; a wrong plan costs time and cannot change
  the result.

Measured on an Arc A380 (xe driver, Level Zero, icpx 2026.0) at
`--precision max` against `--backend cpu`, frames identical and the largest
difference:

| Fixture | Before | Now |
|---|---|---|
| Netflix 576x324, 48 frames | 0 of 48, 1.1e-12 | 48 of 48 |
| Checkerboard 1920x1080, 1 px shift, 3 frames | 0 of 3, 2.6e-13 | 3 of 3 |
| Checkerboard 1920x1080, 10 px shift, 3 frames | 0 of 3, 7.6e-11 | 3 of 3 |
| BBB 3840x2160, 200 frames | 0 of 200, 7.2e-13 | 200 of 200 |
| Netflix 576x324 at 10, 12 and 16 bits and 4:2:2 10-bit, 3 frames each | 0 of 3, 1.0e-12 | 3 of 3 |

Also identical: `yuv_matrix` 1, 2 and 3 on the Netflix pair and the 10 px
checkerboard. What each cause contributed, from the new twin with one piece
put back (Netflix, 1 px checkerboard, 10 px checkerboard, BBB):

| Piece put back | Largest difference |
|---|---|
| Each term the old pair of `float` values, added in the CPU's order | 1.1e-12, 2.0e-13, 2.7e-12, 1.0e-12 |
| The exact terms, added per 512-pixel chunk and the chunk sums in order | 1.3e-13, 3.4e-13, 7.2e-11, 4.5e-13 |

The reference was a GCC build of the CPU extractor; the CPU extractor of the
icx build gives the same scores on these frames. The last step of the score
is the host math library's `pow()`, Intel's in an icx build and glibc's in a
GCC build; a difference there would be between the two CPU extractors, and
none was seen.

Through the `vmaf` tool on the A380 a 3840x2160 frame takes 194.7 ms, 84.1 ms
before (medians of 7 runs of 20 frames; the `float_psnr` control read 3.26 and
3.27 ms), and a 576x324 frame 13.5 ms, 5.3 before. The CPU extractor takes
about 125 ms and 1.4 ms on sixteen threads, so on this card it is now the
faster path at 3840x2160 as well. By builds with stages removed, 69.1 ms are
the stages the twin had before (upload, conversion, XYB, blurs, downsample),
12.8 the `float` advice sums, 7.9 the plan, 81.7 the terms in integer
arithmetic and their steps, and 20.0 the walk; at 576x324 the walk is 5.5 of
the 13.5 ms, because it runs on one lane per sum and one lane of this device
is slow. The twin holds 18 MB more device memory at 3840x2160 and launches six
kernels per scale where it launched two. No kernel uses [scratch
memory](#scratch-memory-on-intel-gpus-adr-1395). Getting the time back is
`T-SYCL-SSIMULACRA2-EXACT-THROUGHPUT-2026-10-02` in
[`state.md`](../../state.md).

The parity gate compares the twin with tolerance 0
([cross-backend gate](../../development/cross-backend-gate.md)), and the Arc
A380's 5e-2 calibration for this feature is gone.

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py \
    --backend sycl --vmaf "$PWD/build/tools/vmaf" --feature ssimulacra2
```

## `float_moment_sycl` matches the CPU `float_moment` at 16 bits (2026-10-02)

`float_moment_sycl` returns the CPU extractor's four moments bit for bit
([ADR-1449](../../adr/1449-sycl-float-moment-cpu-float-squares.md), after
ADR-1447 for the HIP twin). The CPU forms each sample's square in `float`
before adding it. Up to 12 bits that is the integer square; at 16 bits it is
the square rounded to 24 bits. The kernel added exact integer squares, so its
second moments were the CPU's up to 12 bits and not at 16. It now adds the
`float` square, an integer below 2^32, into the same `int64` sums.

A sweep of every SYCL twin on fixtures the parity gate does not use found it:
the repository's 16-bit Netflix clip is 8-bit content shifted left, whose
squares have few significant bits. Measured on an Arc A380 at
`--precision max` against `--backend cpu`, frames whose second moments are
identical and the largest difference:

| Fixture | Before | Now |
|---|---|---|
| Netflix 576x324 at 8 to 16 bits and 4:2:2, both 1080p checkerboards, BBB 3840x2160, noise at 8, 10 and 12 bits | 275 of 275 | 275 of 275 |
| Full-range noise 576x324, 16 bit, 3 frames | 0 of 3, 2.7e-5 | 3 of 3 |
| Bright 16-bit 1920x1080 (samples 56000 to 64000), 2 frames | 0 of 2, 1.0e-4 | 2 of 2 |
| BBB 3840x2160 widened to 16 bits, 8 frames | 0 of 8, 3.9e-5 | 8 of 8 |

The sums are exact integers, and so is the CPU's running `double` sum while
it is below 2^53 units of 2^-16. That covers every frame of up to 2 097 152
pixels and every 8-, 10- and 12-bit frame. On a larger 16-bit frame whose
sum of squares passes 2^53 the CPU rounds each further add, and the twin,
which rounds once, is within a derived bound of it (2.7e-7 measured on a
2560x1440 frame, bound 6.6e-6). Since 2026-10-03 that range is bit-identical
too ([below](#float_moment_sycl-matches-the-cpu-float_moment-past-253-units-too-2026-10-03)).

The frame time is unchanged: 28.7 ms per 3840x2160 frame and 0.6 ms per
576x324 frame on the A380, before and after. Most of the 28.7 ms is the
reduction, four atomic adds per pixel
(`T-SYCL-FLOAT-MOMENT-PER-PIXEL-ATOMICS-2026-10-02`). The parity gate
compares the twin with tolerance 0.

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py \
    --backend sycl --vmaf "$PWD/build/tools/vmaf" --feature float_moment
```

## `float_moment_sycl` matches the CPU `float_moment` past 2^53 units too (2026-10-03)

`float_moment_sycl` now returns the CPU extractor's four moments bit for bit
on every frame
([ADR-1497](../../adr/1497-float-moment-twins-cpu-sum-past-2-53.md)).
The CPU adds the float squares into one `double` in raster order. Below 2^53
units of 2^-16 that sum is exact and equal to the twin's integer sum, which
covers every frame of up to 2 097 152 pixels and every 8-, 10- and 12-bit
frame. On a larger 16-bit frame whose sum passes 2^53 the CPU rounds as it
adds, and the twin used to round the exact sum once. It now forms the CPU's
rounded sum: on such a frame four more kernels add each row exactly while the
sum is at or below 2^53, then from integer increments of the sum's last
place, composed in pixel order and checked against the exact running sum,
and term by term where a row crosses into the next binade
(`core/src/feature/float_moment_sum.h`). Frames that cannot pass 2^53 run no
new work.

Measured on an Arc A380 at `--precision max` against `--backend cpu`, frames
whose four outputs are identical and the largest difference:

| Fixture | Before | Now |
|---|---|---|
| Full-range 16-bit noise 3840x2160, nine tenths near the peak, 16 frames | 0 of 16, 2.7e-7 | 16 of 16 |
| The same at 7680x4320, 4 frames | 0 of 4, 5.1e-7 | 4 of 4 |
| BBB 3840x2160 widened to 16 bits (shifted left by 8, times 257, full range with a dithered low byte), 32 frames each, 17 of them past 2^53 | 32 of 32 | 32 of 32 |

BBB was identical before as well: its widened samples have no bits below the
sum's last place until 2^55, which a 3840x2160 frame cannot reach. The parity
gate's `float_moment` cell reads 0 at tolerance 0 on the 16-bit 3840x2160
noise (it failed there before).

Time per 16-bit 3840x2160 frame through libvmaf, pictures preloaded, medians
of 5 interleaved runs at a load average of 4 to 5, before and after:

| Input | Before | After |
|---|---|---|
| Noise, every frame past 2^53 | 31.84 ms | 34.62 ms |
| BBB full range, 17 of 32 frames past 2^53 | 32.05 ms | 33.02 ms |

The four kernels use integers only and no scratch memory
(`test_sycl_kernel_scratch`), and compile for every ahead-of-time target.

```shell
python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build/tools/vmaf \
    --reference ref_16bit_3840x2160.yuv --distorted dis_16bit_3840x2160.yuv \
    --width 3840 --height 2160 --bitdepth 16 --backends cpu sycl --features float_moment
```

## `float_psnr_sycl` matches the CPU `float_psnr` at every bit depth (2026-10-02)

`float_psnr_sycl` returns the CPU extractor's score bit for bit
([ADR-1450](../../adr/1450-sycl-float-psnr-exact-block-sums.md), after
ADR-1440 for the HIP twin). The CPU squares each sample difference in `float`
and adds the squares in `double`, which does not round. The kernel formed the
same squares and added each 16x16 work-group in `float`: exact at 8 bits, and
at 10, 12 and 16 bits only while a group's differences are small. It now
converts each `float` square to an integer and the sub-groups, the
work-groups and the host add `uint64` values.

Found by the same sweep as the `float_moment` defect above; the repository's
high-bit-depth clips are 8-bit content shifted left and show nothing.
Measured on an Arc A380 at `--precision max` against `--backend cpu`:

| Fixture | Before | Now |
|---|---|---|
| Netflix 576x324 at 8 to 16 bits and 4:2:2, both 1080p checkerboards, BBB 3840x2160, noise at 8 bits | 269 of 269 | 269 of 269 |
| Full-range noise 576x324 at 10, 12 and 16 bits, 3 frames each | 0 of 9, 2.4e-8 dB | 9 of 9 |
| Bright 16-bit 1920x1080 (samples 56000 to 64000), 2 frames | 0 of 2, 7.4e-8 dB | 2 of 2 |
| BBB 3840x2160 widened to 16 bits, 8 frames | 0 of 8, 3.3e-8 dB | 8 of 8 |

The same holds with `uncapped=true`. The reference in that table is the CPU
extractor of the same build, which is what the parity gate compares. Against
a GCC build 6 of the 288 frames differ by at most 1.4e-14 dB: the noise is
equal and the host's `log10` is Intel's in one build and glibc's in the
other.

At 16 bits the CPU's own sum is exact up to a mean squared error of
2^37 / (width x height) on the 8-bit scale (a PSNR below 6 dB at 3840x2160).
Beyond it the CPU rounds as it adds the rows and the twin is within 7e-13 dB.

A 3840x2160 frame takes 3.41 ms, 3.31 ms before (medians of 7 runs; the
`psnr_sycl` control read 2.93 and 2.94 ms); a 576x324 frame 0.12 ms, as
before. The read-back is 8 bytes per work-group where it was 4. The parity
gate compares the twin with tolerance 0.

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/dev/speed_gpu_parity.py \
    --backend sycl --vmaf "$PWD/build/tools/vmaf" --feature float_psnr
```

## Exact twins declared as a group (2026-10-02)

`adm_sycl`, `motion_sycl`, `motion_v2_sycl`, `psnr_sycl`, `float_ssim_sycl`
and `cambi_sycl` returned the CPU's bits already and were compared with a
tolerance of 5e-5. They are declared exact twins now
([ADR-1451](../../adr/1451-sycl-exact-twins-declared.md)): the parity gate
compares `adm`, `motion`, `motion_debug`, `motion_v2`, `psnr`, `float_ssim`,
`float_ssim_lcs` and `cambi` with tolerance 0, and `test_sycl_exact_twins`
holds every output of the six twins to `==` on a device.

A twin is listed when it reaches the CPU's value by construction and a sweep
measured it identical. The sweep, on an Arc A380 at `--precision max`: the
Netflix 576x324 pair at 8, 10, 12 and 16 bits and as 10-bit 4:2:2, both
1920x1080 checkerboard pairs, full-range noise at four bit depths, a bright
16-bit 1920x1080 pair, BBB 3840x2160 widened to 16 bits and 200 frames of BBB
3840x2160. Every one of the 333 frames was identical for each of the eight
features, and `float_ssim` at `scale=1` on 50 frames of BBB 3840x2160.

Two features are not listed:

- `speed_chroma` was identical on all 333 frames as well. Its `log2` was a
  correctly rounded evaluation on the device, where the CPU calls the math
  library of the build, so the equality depended on that library. Since
  [ADR-1477](../../adr/1477-speed-upstream-double-math.md) the twin's
  logarithms are the host's and `speed_chroma` and `speed_temporal` are
  listed (`scripts/ci/exact_twins.d/speed_chroma.sycl`,
  `speed_temporal.sycl`).
- `ciede` is within 1.4e-11 of the CPU by its derived bound (ADR-1436).

One of the listed twins is exact within a stated range: `cambi` while
`cambi.c`'s own top-K sum is exact (ADR-1357). `float_ssim` was listed as
exact up to the `float` rounding of the frame mean; a constructed frame
showed that this is not exact, and since
[ADR-1463](../../adr/1463-sycl-float-ssim-raster-sum.md) the twin adds the
CPU's terms in the CPU's order
([below](#float_ssim_sycl-adds-its-frame-sums-in-the-cpus-order-2026-10-02)).

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary build/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --backends cpu sycl
```

## `float_ssim_sycl` adds its frame sums in the CPU's order (2026-10-02)

`float_ssim_sycl` returned the CPU's `float_ssim` on every frame of real
content measured and was declared an exact twin. It was not exact on every
input: on a constructed 64x64 pair the CPU scores -4.222829943500983e-07 and
the twin scored -4.222829659283889e-07, the next `float`
([ADR-1463](../../adr/1463-sycl-float-ssim-raster-sum.md)).

The CPU adds one term per window into a `double`, window after window, and
that running sum rounds at every add. The twin added the terms as integers
per work-group, which is another sum of the same windows: more accurate, and
not the CPU's. The two differ around 1e-16 relative, and the result is
rounded to `float`, so the difference shows only when the mean falls that
close to a rounding boundary. That happens on pictures whose terms cancel,
such as independent noise: 3 frames in 27 million at 64x64 in a search. None
of the 333 frames of the sweep above differs.

The twin now computes each window's luminance and contrast terms as the
CPU's `double` values (in 64-bit integers, because a SYCL kernel has no
`double`), stores the term of every window and lets the host add them in the
CPU's order. With `enable_lcs` it stores the three terms and the host forms
the four sums.

Measured on an Arc A380 (xe) at `--precision max` against a GCC build of the
CPU extractor: the constructed pair is identical, with and without
`enable_lcs`; so are 2070 of 2070 values on 138 frames (the fixtures of the
sweep above, BBB 3840x2160 with 50 frames) under six option sets: default,
`enable_lcs`, `scale=1`, `enable_lcs` with `scale=1`, `scale=3`, and
`enable_lcs` with `scale=2`.

What it costs depends on the scale. `float_ssim` shrinks the picture first
(to at most 480x270 for 1080p and 4K input unless `scale` says otherwise),
and the number of terms is the number of pixels after that:

| Input, options | Before | After |
|---|---|---|
| 1920x1080, automatic scale | 1.32 ms | 1.51 ms |
| 1920x1080, `enable_lcs` | 1.34 ms | 1.60 ms |
| 3840x2160, automatic scale | 3.93 ms | 4.11 ms |
| 3840x2160, `enable_lcs` | 4.25 ms | 4.35 ms |
| 576x324 (scale 1) | 0.56 ms | 0.86 ms |
| 1920x1080, `scale=1` | 5.9 ms | 9.9 ms |
| 1920x1080, `scale=1`, `enable_lcs` | 6.5 ms | 12.4 ms |
| 3840x2160, `scale=1` | 23.3 ms | 39.1 ms |
| 3840x2160, `scale=1`, `enable_lcs` | 25.5 ms | 48.6 ms |

Per frame, medians of 7 interleaved runs of 50 frames (host load average 22
to 26; a control with the same code in both builds read 2.93 and 2.92 ms).
At 3840x2160 with `scale=1` the 16 ms are 7 ms of kernel arithmetic, 6 ms
for reading 66 MB back and 3 ms of host additions; the row
`T-SYCL-FLOAT-SSIM-RASTER-SUM-THROUGHPUT-2026-10-02` in
[`state.md`](../../state.md) holds the split and the candidates. The twin
also holds 8 bytes per window on the device and in pinned host memory (20
with `enable_lcs`): 66 MB (165 MB) at 3840x2160 with `scale=1`, 1 MB (2.4 MB)
at the automatic scale.

To check a build, score the constructed pair. `core/test/float_ssim_order_frame.h`
holds it as two C arrays; the Python lines below regenerate the same bytes:

```bash
python3 -c "
import random
n = 6144
for name, seed in (('ref', 174), ('dis', 175)):
    open(f'order_{name}_64x64.yuv', 'wb').write(random.Random(seed).randbytes(20000 * n)[14161 * n:14162 * n])"
for b in cpu sycl; do
  vmaf -r order_ref_64x64.yuv -d order_dis_64x64.yuv -w 64 -h 64 -p 420 -b 8 \
    --no_prediction --feature float_ssim --backend "$b" --precision=max --json -q -o "order_$b.json"
done
python3 -c "import json; print([json.load(open(f'order_{b}.json'))['frames'][0]['metrics']['float_ssim'] for b in ('cpu', 'sycl')])"
```

It prints `[-4.222829943500983e-07, -4.222829943500983e-07]`; before, the
second value was `-4.222829659283889e-07`.

`float_ms_ssim_sycl` computed its per-scale means the old way and had the
same defect; it follows
[below](#float_ms_ssim_sycl-adds-its-per-scale-sums-in-the-cpus-order-2026-10-02).

## `float_ms_ssim_sycl` adds its per-scale sums in the CPU's order (2026-10-02)

`float_ms_ssim` calls the same routine as `float_ssim` once per scale and
uses its luminance, contrast and structure means, so the defect of the
previous section applied to it: the twin added each scale's terms as
integers per work-group, an exact sum where the CPU keeps a running `double`
that rounds. On a 176x176 pair of noise the twin's `float_ms_ssim_l_scale0`
was 0.9884905219078064 where the CPU's is 0.9884904623031616, the next
`float` down.

The twin now stores `l`, `c` and `s` of every window of every scale, `l` and
`c` as the CPU's `double` values, and the host adds them in the CPU's order
([ADR-1466](../../adr/1466-sycl-float-ms-ssim-raster-sum.md)). The pair
arithmetic for `l` and `c` and the fixed-point sums are gone from
`core/src/feature/sycl/sycl_ssim_terms.h`; both SSIM twins use the same
functions.

A second pair, found by the HIP lane and shared by the tests of all three
backends (`core/test/float_ms_ssim_order_frame.h`), moves
`float_ms_ssim_c_scale1` on the CUDA and HIP twins. The SYCL twin returned the
CPU's value on it before this change and does after: its old sum was exact,
and on that pair the exact sum rounds to the CPU's `float`.

Measured on an Arc A380 (xe) at `--precision max` against a GCC build of the
CPU extractor, on 138 frames (the fixtures of the previous section):
`float_ms_ssim` 138 of 138, with `enable_lcs` 2208 of 2208 values. With
`enable_chroma`, on the 69 frames whose chroma planes are large enough, 206
of 207 values; the one that differs does so by 1.1e-16 through the host's
`pow()`, which an icx build takes from Intel's math library, and the CPU
extractor of the same binary differs from the GCC build on that value too.

| Input, options | Before | After |
|---|---|---|
| 3840x2160 | 44.7 ms | 75.9 ms |
| 3840x2160, `enable_lcs` | 44.9 ms | 76.0 ms |
| 3840x2160, `enable_chroma` | 65.2 ms | 112.4 ms |
| 1920x1080 | 11.5 ms | 18.2 ms |
| 1920x1080, `enable_lcs` | 11.3 ms | 19.2 ms |
| 576x324 | 1.16 ms | 1.92 ms |

Per frame, medians of 7 interleaved runs of 25 frames, host load average 13
to 15; a control with the same code in both builds read 2.95 and 2.92 ms.
The CPU extractor takes 409 ms per 3840x2160 frame on one thread and 58.7 ms
with 16 threads, so at that size the twin is now slower than a 16-thread CPU
run; `--backend cpu` is the faster choice for 4K `float_ms_ssim` on this
device until the tuning row is worked. Of the extra time at 3840x2160 about
8 ms are kernel arithmetic, 21 ms the copy of 219 MB of terms to the host
and 5 ms the host's additions; the row
`T-SYCL-FLOAT-MS-SSIM-RASTER-SUM-THROUGHPUT-2026-10-02` in
[`state.md`](../../state.md) holds the split and the candidates. The twin
holds 20 bytes per window on the device and in pinned host memory: 219 MB at
3840x2160 (327 MB with `enable_chroma`), 54 MB at 1920x1080.

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary build/tools/vmaf --features float_ms_ssim float_ms_ssim_lcs \
    --backends cpu sycl \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324
```

## Licensing of the SYCL kernels (ADR-1250)

As with the other backends, a SYCL kernel implementing an upstream Netflix
metric keeps that code's terms and copyright notice, while fork-original SYCL
code is EUPL-1.2. Four files in `core/src/sycl/` additionally carry an outside
contributor's work and stay on their current terms until that contributor
agrees to a change. See [ADR-1250](../../adr/1250-eupl-fork-relicense.md).
