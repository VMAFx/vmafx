# Research-2123: Arc B580 SYCL crashes — psnr_hvs compile, tile-halo faults

- **Status**: Active
- **Workstream**: bug fixes (`T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29`,
  `T-SYCL-TILE-HALO-OOB-READ-2026-09-29`,
  `T-SYCL-GRAPH-WAIT-ERROR-DROPPED-2026-09-29`,
  `T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29`,
  `T-SYCL-VIF-ODD-WIDTH-RD-STRIDE-2026-09-29`) and
  [ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md)
  (`T-SYCL-PSNR-HVS-4K-PARITY-GATE-2026-09-29`)
- **Last updated**: 2026-09-29

## Question

Two SYCL tests fail on an Intel Arc B580 (`bmg-g21`, Xe2) on master
`b3ae47505`: `test_sycl_psnr_hvs_parity` (and its `_large` build) dies with
SIGSEGV, and `test_sycl_adm_tiny_frames` reports a wrong score. The UHD 770
(`adl-s`, Xe-LP) runs psnr_hvs correctly. What is the root cause of each, are
they the same class of defect, and does the same cause exist in other SYCL
kernels?

## Sources

- Host: Windows 11 + WSL2 (`--device /dev/dxg`), `vmaf-dev-mcp:local`, oneAPI
  2026.1 (`libsycl.so.9`, Level Zero v2 adapter), compute-runtime
  26.35.39758.10, IGC 2.41.5. `ONEAPI_DEVICE_SELECTOR=level_zero:0` is the
  B580 (sub-groups 16/32), `level_zero:1` the UHD 770 (sub-groups 8/16/32).
  Build: `-Denable_sycl=true -Dsycl_icpx_aot_targets=bmg-g21,adl-s
  --buildtype=release -Db_lto=false`, icx/icpx.
- Tools: `gdb-oneapi` (batch backtrace, `INTELGT_AUTO_ATTACH_DISABLE=1`),
  `SYCL_UR_TRACE=2`, `SYCL_DUMP_IMAGES=1`, IGC `IGC_ShaderDumpEnable=1` and
  `IGC_ForceOCLSIMDWidth`, `SYCL_UR_USE_LEVEL_ZERO_V2=0 UR_L0_SERIALIZE=2`, and
  a scratch build that waits after every ADM kernel.
- Code: `core/src/feature/sycl/integer_psnr_hvs_sycl.cpp`,
  `core/src/feature/sycl/integer_adm_sycl.cpp`, the CPU reference
  `core/src/feature/third_party/xiph/psnr_hvs.c`.

## Findings

### 1. psnr_hvs: the GPU compiler crashes at SIMD32 on Xe2

The SIGSEGV is not in libvmaf. The backtrace ends in `libigc.so.2` below
`ur::level_zero::urProgramBuildExp`, called from
`sycl::detail::ProgramManager::getBuiltURProgram` on the first `q.submit()`:
the driver's compiler crashes while JIT-compiling the kernel. The UR trace shows
`urProgramCreateWithIL` with the kernel's 23636-byte SPIR-V module on both
devices. With `IGC_ShaderDumpEnable=1` the B580 compile writes
`*_simd32_entry_0001.visaasm` (11 719 lines, `Platform: XE2`, built with
`-abortonspill`) and dies before the final `.asm`, so the crash is in the vISA
finalizer's SIMD32 compile. The UHD 770 compiles the same module at SIMD32
(8386 instructions, 330 flag-register spills) without trouble.

| Knob on the B580 | Result |
| --- | --- |
| default | SIGSEGV |
| `IGC_ForceOCLSIMDWidth=32` | SIGSEGV |
| `IGC_ForceOCLSIMDWidth=16` | pass |
| `-ze-opt-level=1`, `-ze-opt-large-register-file`, `-ze-intel-enable-auto-large-GRF-mode` | SIGSEGV |
| `-ze-opt-disable` | pass |

The UHD 770 passes with SIMD 8, 16 and 32 forced. The trigger is the kernel's
shape: after the cooperative load, work-item 0 alone ran the whole 8x8 integer
DCT and masking with five private 64-element arrays (`int ref[64]`,
`int dist[64]`, the DCT's `int z[64]` twice, `float mask[64]`). At SIMD32 each
of those private words occupies a lane of 32, and IGC 2.41.5's Xe2 SIMD32
compile of that footprint crashes instead of spilling or falling back.

The fix removes the footprint rather than steering the compiler: the DCT
moves into local memory, one 1-D transform per work-item and pass (items 8-23
run the first pass of the ref and dist blocks while items 0 and 1 take the two
variance ratios; items 0-15 run the second), and work-item 0 streams the
coefficients from local memory for the three float reductions, computing
`mask[i][j]` per coefficient. The integer DCT is exact and the float
reductions keep the CPU's `i, j` order, so the output is bit-identical to the
old kernel. The new module compiles at SIMD16 by default on the B580 (3459
instructions) and at SIMD8 on the UHD 770, and also compiles with SIMD32
forced on both.

### 2. integer ADM on tiny frames: an out-of-bounds tile load faults the device

This is a different defect, and it is not B580-specific: the test fails the
same way on both GPUs (17x17 `integer_adm_scale0` CPU 0.99526475, SYCL
1.00000002). The SYCL run logs
`SYCL graph wait: ... UR_RESULT_ERROR_DEVICE_LOST`
and then still emits scores: the accumulators never received the frame, so
every scale's numerator equals its denominator (finding 3). Frames 96x64,
64x64, 48x48, 33x33, 32x32, 24x24 and 17x17 all fault; 576x324 does not.
Waiting after every kernel shows the first failure at scale 3's
`launch_dwt_vert_pair`, on both devices.

`launch_dwt_vert_pair` loads a fixed 18-row tile per work-group
(`row_start = 2 * n_start - 1` to `+ 16`) for all 8 output rows of the group,
including the padding rows past `half_h`, and reflects out-of-plane rows once
with `dev_mirror_adm()` (`idx >= sup -> 2 * sup - idx - 1`). A valid output row
`n` reads input rows `2n-1 .. 2n+2`, inside `[-1, h + 1]`, which one reflection
covers. Row 16 of the tile does not: on a plane of 8 rows it reflects to -1, on
the 3-row scale-3 plane of a 17x17 frame to -11. The load reads before the start
of the band's USM allocation and page-faults whenever the preceding page is
unmapped. Every frame 64 rows high or less reaches it at scale 3 (its input is
`ceil(h / 8)` rows). The values are never consumed, so the defect only shows as
a fault, and only with an unfavourable allocation layout: the test passed on an
Arc A380 when it landed (`T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18`).

The fix clamps the reflected index into the plane
(`core/src/feature/sycl/sycl_tile_index.h`, `vmaf_sycl_tile_index()`). It is
the identity for every consumed row, so scores cannot move: the default model
plus `psnr_sycl` and `float_moment_sycl` is bit-identical to master on the
UHD 770 on the Netflix pair and 50 frames of BBB 4K.

### 3. A failed graph wait did not fail the frame

`vmaf_sycl_graph_wait()` is idempotent per frame: the first caller waits and
the rest return early. It returned 0 to the later callers even when the wait
had failed, and all five graph collectors (`adm_sycl`, `vif_sycl`,
`motion_sycl`, `psnr_sycl`, `float_moment_sycl`) ignored its result anyway.
After a device fault they scored the faulted frame from stale host buffers and
`vmaf_read_pictures()` returned 0; only the next frame's upload wait failed,
so a one-frame run exited 0. The wait now marks the frame waited only after it
succeeds, so each collector's call waits again on the lost device and fails,
and every collector returns that result (-EIO): the faulted frame itself
fails. Before finding 6, `vif_sycl` on an 8x8 frame still faulted; with this
change it stopped at frame 0 with `problem reading pictures`.

### 4. The same tile-halo pattern elsewhere

A tiny-frame sweep (noise, 3 frames, each size in its own process) on master:

| Extractor | UHD 770 | B580 |
| --- | --- | --- |
| `adm_sycl` | device lost, 17x17 to 96x64 | device lost, 17x17 to 96x64 |
| `vif_sycl` | device lost, 3x3 to 8x8 | device lost, every size tested (3x3 to 96x64) |
| `motion_sycl` | clean | device lost, 3x3 to 33x33 |
| `motion_v2_sycl`, `float_motion_sycl`, `float_vif_sycl`, `float_adm_sycl` | clean | clean |

`integer_vif_sycl.cpp` (`dev_vert_load_tile`, `dev_fused_load_tile`),
`integer_motion_sycl.cpp` (`motion_load_tile`), `integer_motion_v2_sycl.cpp`
(`mv2_load_diff`), `float_motion_sycl.cpp` (`fm_load_tile`) and
`float_vif_sycl.cpp` (`load_vif_tile`) load a fixed tile of work-group size
plus halo with a single reflection, like the ADM kernel, so their padding lanes
reach outside the plane on small frames; the three clean rows are faults that
did not happen to land on an unmapped page. All five loaders now pass the
reflected index through `vmaf_sycl_tile_index()`. `float_adm_sycl.cpp`
already clamps its halo, `integer_ms_ssim_sycl.cpp` reflects with a true
period-2n mirror, and `integer_cambi_sycl.cpp` skips out-of-plane taps.

### 5. The AOT option does not reach the binaries

`SYCL_DUMP_IMAGES=1` finds only `sycl_spir64*.spv` images in the test
executables, and the UR trace shows `urProgramCreateWithIL` for every kernel on
both GPUs: `-Dsycl_icpx_aot_targets` compiles objects with
`-fsycl-targets=spir64_gen,spir64`, but `sycl_dependency` links with `-fsycl`
alone, so the device link produces SPIR-V only and every run JIT-compiles.
That is why the crash is in the runtime's compiler rather than at build time.
Recorded as `T-SYCL-AOT-TARGETS-DROPPED-AT-LINK-2026-09-29`; not changed here.

### 6. Integer VIF below 16 pixels: fall back to the CPU (maintainer decision)

After the clamp the sweep was clean everywhere except `vif_sycl` at 3x3, 5x5
and 8x8 on both GPUs. There the taps a valid output consumes are themselves more
than one reflection outside the plane (a 17-tap scale-0 filter on an 8-wide
plane), in per-output paths such as `dev_hori_convolve_border()`, not in a
tile loader. Scale s filters floor(dim / 2^s) samples and reflects each tap
once, which stays inside while floor(dim / 2^s) ≥ half-width + 1: {9, 10, 12, 16}
for the {17, 9, 5, 3} filters and {5, 6, 8} for the decimation filters. The
kernel's real bound is therefore 16 pixels in each dimension, the value
`vif_get_min_dim()` gives float VIF for the same widths. The CPU `vif` accepts
and scores smaller frames without faulting.

On 2026-09-29 the maintainer chose to fall back to the CPU. `vif_sycl` declares
`VIF_MIN_DIM` (computed from its filter tables, `static_assert` 16) through the
[ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md) first-picture
gate: `context_check` returns `-ENOTSUP` below it and names `vif` as the
fallback, so model dispatch, and the CLI twin selection in #1619 that consults
the gate, compute those frames on the CPU. A direct `vif_sycl` request fails
`init()` with `-EINVAL`. `test_sycl_vif_min_dim` registers `vmaf_v0.6.1`'s
four VIF features on a SYCL context: below 16 the scores equal the CPU's bit
for bit; at 16x16, 17x17, 96x64 and 853x480 they stay on the device within
2.6e-8 of the CPU, on both GPUs. `motion_sycl`, `motion_v2_sycl` and
`float_motion_sycl` need no declaration: their 5-tap filters need 3 pixels,
which their `init()` requires as the CPU does, and the sweep is clean from 3x3
up. `float_vif_sycl` already refuses the same sizes the CPU `float_vif` does.

### 7. Integer VIF read odd-width scales at the wrong stride

The first run of that test failed above the bound: at 17x17
`integer_vif_scale1` was 0.0765 on the CPU and 0.0962 on the device.
`dev_downsample_rd()` writes the next scale at stride ceil(w / 2)
([ADR-1034](../adr/1034-sycl-vif-rd-stride-motion-uv-sync.md)), but
`enqueue_vif_work_impl()` read it back at stride floor(w / 2),
so every row after the first was skewed as soon as one scale's width was odd.
Worst scale difference from the CPU on master, ramp-plus-noise frames, UHD 770:

| Size | Worst scale | Max difference |
| --- | --- | --- |
| 17x17 | scale 3 | 3.3e-2 |
| 33x33 | scale 2 | 6.6e-3 |
| 257x145 | scale 1 | 1.4e-3 |
| 853x480 | scale 1 | 1.2e-3 |
| 854x480 | scale 3 | 9.3e-4 |
| 1366x768 | scale 3 | 7.9e-4 |
| 576x324, 1920x1080 | — | ≤ 2.5e-7 |

Every fixture in the tree has an even-width ladder, which is why none caught
it. The next scale now reads at the stride it was written with; every size in
the table is within 2.5e-5 of the CPU, 6.4e-7 or better from 257x145 up.
`float_vif_sycl`, `float_adm_sycl` and `adm_sycl` were checked at 33x33,
257x145, 853x480 and 1366x768 and stay within 3e-5 (2.1e-6 or better from
257x145 up); `float_ms_ssim_sycl` within 4e-8 at 853x480 and 1366x768 (both
backends refuse the two smaller sizes). CUDA and HIP keep one `rd_stride` for
writer and reader (read, not run).

The same comparison shows `motion_sycl` 1.22e-4 from the CPU at 17x17
(motion2 5.75), shrinking to 4e-6 at 576x324, identical on master and on this
branch; its root cause is open (`T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29`).

### 8. psnr_hvs gate tolerance scales with the CPU's sum length (maintainer)

On 2026-09-29 the maintainer chose an area-scaled tolerance over emulating the
CPU's summation order. [ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md)
derives it: the CPU's single float sum over N luma terms has relative error
about λ · 2⁻²⁴ · √N, which is (10 / ln 10) times that in dB; λ = 3.93 keeps
576x324 at today's 5e-4, so the tolerance is 5e-4 · max(1, √(N / N₅₇₆ₓ₃₂₄)):
3.34e-3 at 3840x2160. Both gates apply `area_tolerance_factor()`. Run through
the real gate on both GPUs: 576x324 8.3e-5 against 5e-4 and 3840x2160
8.43e-4 against 3.34e-3, both OK. The 4K fixture also crashed the gates with
TypeError: identical chroma planes give `psnr_hvs_cb` = inf, which vmaf writes
as JSON `null` (four of the 50 frames); `metric_delta()` treats `null` on both
sides as agreement.

### Measurements after the fix

- `test_sycl_psnr_hvs_parity`, `_large` and the new `_simd32`, and
  `test_sycl_adm_tiny_frames`: pass on both GPUs; the whole `sycl` suite
  (50 tests) passes on both.
- `vmaf --backend sycl --feature psnr_hvs_sycl` against CPU `psnr_hvs`, worst
  frame, identical on both GPUs: Netflix pair (48 frames) `psnr_hvs` 8.03e-5,
  `psnr_hvs_y` 8.37e-5, `_cb` 2.89e-5, `_cr` 3.91e-5 (gate 5e-4); BBB 4K
  (50 frames) `psnr_hvs` 7.63e-4, `psnr_hvs_y` 8.42e-4, `_cb` 1.59e-4,
  `_cr` 1.47e-4. The 4K figures exceeded the fixed 5e-4 gate on master too
  (bit-identical output); finding 8 scales the gate.
- 4K `t(22) - t(2)`, median of three: `psnr_hvs_sycl` on the UHD 770
  208 ms/frame on master, 130 ms/frame now; 20.7 ms/frame on the B580 (master
  crashes). `adm_sycl` on the UHD 770 36.2 and 36.0 ms/frame.

## Alternatives explored

- **Pin the psnr_hvs kernel to SIMD16** with `VMAF_SYCL_REQD_SG_SIZE(16)`.
  Avoids the crashing compile on this driver, but leaves a kernel that keeps
  63 of its 64 work-items idle through the DCT and a private footprint any other
  compiler or width can trip over; the attribute is also a no-op under
  AdaptiveCpp. Rejected in favour of removing the footprint, which is also
  faster.
- **Force AOT compilation** so the crash would appear at build time. It would
  move, not remove, the defect (ocloc runs the same IGC), and the AOT path is
  itself broken (finding 5).
- **Stop loading unconsumed tile rows** (per-row validity masks in each loader)
  instead of clamping. Equivalent, but more code per loader and a branch on the
  hot boundary path; the clamp is one shared helper.
- **Accumulate the psnr_hvs block partials in double** on the host, to close
  the 4K gap. It moved SYCL further from the CPU (`psnr_hvs` 6.4e-3 at 4K),
  which shows the gap comes from the CPU reference's own float accumulation.
  Not adopted.

## Open questions

- The CUDA and HIP ADM vertical DWT (`adm_dwt2_load_column`) reflect the
  bottom edge once with `y_in - max(0, 2 * (y_in - h) + 1)` and load before
  any row check, the same pattern; not run here
  (`T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29`).
- The CUDA, HIP and Metal integer VIF twins declare no 16-pixel minimum
  (`T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29`); not run here.
- `motion_sycl` tiny-frame parity (finding 7,
  `T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29`).

## Related

- Rows: `T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29` (opened in #1619),
  `T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18`,
  `T-SYCL-TILE-HALO-OOB-READ-2026-09-29`,
  `T-SYCL-GRAPH-WAIT-ERROR-DROPPED-2026-09-29`,
  `T-SYCL-PSNR-HVS-4K-PARITY-GATE-2026-09-29`,
  `T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29`,
  `T-SYCL-VIF-ODD-WIDTH-RD-STRIDE-2026-09-29`
- ADRs: [ADR-0220](../adr/0220-sycl-fp64-fallback.md) (fp64-free kernels),
  [ADR-0191](../adr/0191-psnr-hvs-vulkan.md) (psnr_hvs GPU design and tolerance),
  [ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md) (first-picture
  CPU fallback), [ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md)
  (area-scaled psnr_hvs tolerance)
- PRs: #1619 (CLI twin selection, which routes
  `--backend sycl --feature psnr_hvs` to this kernel)
