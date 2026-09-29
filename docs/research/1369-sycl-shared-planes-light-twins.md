<!-- markdownlint-disable MD013 MD060 -->
# Research-1369: SYCL psnr_hvs, motion_v2 and psnr at 4K — where the frame time went and how the twins now share the uploaded planes

- **Status**: Active
- **Workstream**: [ADR-1369](../adr/1369-sycl-shared-planes-light-twins.md), [ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md), [ADR-0220](../adr/0220-sycl-fp64-fallback.md)
- **Last updated**: 2026-09-29

## Question

On master `2d9d5b069` the SYCL twins of `psnr_hvs`, `motion_v2` and `psnr`
were slower than, or barely faster than, 16 CPU threads at 3840x2160 on an Arc
B580 (19.5, 11.3 and 8.6 ms per frame against 6.95, 7.05 and 6.85). The
maintainer asked for a profile rather than a snapshot ("benching = run it and
find performance if possible, not just take a snapshot") and for no host round
trips ("there shouldnt be any gpu cpu rountrips"). Where does each twin spend
its frame, and what has to change so that every plane crosses the bus once?

## Sources

- Host: Windows 11 + WSL2 (`--device /dev/dxg`), `vmaf-dev-mcp:ocloc`, icpx
  2026.1.1, compute-runtime 26.35.39758.10. `ONEAPI_DEVICE_SELECTOR=level_zero:0`
  is the Arc B580 (`bmg-g21`, Xe2, 160 XVEs, sub-groups 16/32),
  `level_zero:1` the UHD 770 (`adl-s`, Xe-LP, 32 EUs, sub-groups 8/16/32).
  Build: `-Denable_sycl=true -Dsycl_icpx_aot_targets=bmg-g21,adl-s
  --buildtype=release -Db_lto=false`.
- Fixtures: Big Buck Bunny 3840x2160 8-bit 4:2:0 (`ref_3840x2160_200f.yuv` /
  `dis_3840x2160_200f.yuv`), the Netflix `src01_hrc00/01_576x324` pair and its
  10-, 12-bit and 4:2:2 conversions, `sparks_*_480x270` 10-bit.
- Profiling: no `onetrace` / `unitrace` in the image, and VTune GPU analysis
  does not run under WSL2. A scratch build (not committed) recorded
  `sycl::event` profiling times (`command_end - command_start`, with
  `VMAF_SYCL_PROFILE=1`, which gives the primary, combined and — in the scratch
  build only — copy queues `enable_profiling`) for every copy and kernel, and
  `std::chrono` host times around the host-side phases, drained after the
  host wait that already exists in `collect()`. 22 frames, averages per frame.
- Code: `core/src/feature/sycl/integer_psnr_hvs_sycl.cpp`,
  `integer_motion_v2_sycl.cpp`, `integer_psnr_sycl.cpp`,
  `core/src/sycl/common.cpp` (shared frame, combined graph),
  `core/src/libvmaf.c` (`read_pictures_sycl_prep`,
  `dispatch_gpu_double_buffer`), the CPU references
  `third_party/xiph/psnr_hvs.c`, `integer_motion_v2.c`, `integer_psnr.c`.

## Findings

### 1. Where the frame went (master, 3840x2160, ms per frame)

| Phase | psnr_hvs B580 | psnr_hvs UHD 770 | motion_v2 B580 | motion_v2 UHD 770 | psnr B580 | psnr UHD 770 |
| --- | --- | --- | --- | --- | --- | --- |
| Host copy / conversion before upload | 14.48 (float conversion of 6 planes) | 12.38 | 0.97 (luma repack) | 0.99 | 0.86 (chroma repack) | 0.95 |
| Private H2D, device time | 7.18 (99.5 MB of float) | 3.43 | 0.60 | 0.20 | 0.61 | 0.23 |
| Kernels, device time | 6.26 (3 launches) | 103.77 | 0.58 | 13.16 | 0.29 (luma 0.19, chroma 0.10) | 16.12 (luma 10.74, chroma 5.38) |
| Result readback, device time | 0.07 | 0.05 | < 0.01 | < 0.01 | < 0.01 | 0.01 |
| Host wait in `collect()` | 0.66 | 2.31 | 0.08 | 0.01 | 0.06 | 0.02 |
| Host reduction | 0.18 | 0.19 | — | — | — | — |

psnr kernel times are from direct dispatch (`VMAF_SYCL_NO_GRAPH=1`); the graph
replay runs the same kernels. On top of every row, every SYCL run already
uploads the luma of both pictures into the shared frame
(`vmaf_sycl_shared_frame_upload`): 1.9 ms of DMA per 4K frame on the B580
(0.4 on the UHD 770) and 2.2 to 3.0 ms of host time, because the CLI's
pictures live in pageable memory and the driver stages them.

So the three twins each paid for data the device already had, or could have
had once:

- `psnr_hvs_sycl` converted all six planes to `float` on the host
  (`picture_copy_plane`), 24.9 M samples, and uploaded 4 bytes per sample. The
  conversion alone was 14.5 ms of a 19.5 ms frame on the B580. The kernel reads
  the samples back as integers (`sample_to_int`), which is exactly the raw
  sample for 8, 10 and 12 bits.
- `motion_v2_sycl` repacked and re-uploaded the reference luma that the shared
  frame had uploaded moments before.
- `psnr_sycl` read luma from the shared frame but repacked and uploaded its
  own copy of the chroma; `psnr_hvs_sycl` in the same run uploaded the same
  chroma again as floats.

### 2. The kernels

- **psnr_hvs**: one 64-work-item work-group per 8x8 block, and after the
  cooperative DCT all float arithmetic (masking energies, threshold, error sum)
  ran on work-item 0 in the CPU's i, j order. That order is what makes the
  per-block score reproducible, so it has to stay serial per block; but only
  one of 64 lanes did it. 168 784 luma blocks plus 2 x 41 769 chroma blocks per
  4K frame.
- **motion_v2**: every output pixel recomputed its five vertical 5-tap sums
  (25 multiply-adds) in 64-bit integers. Xe-LP has no native 64-bit multiply,
  which is why the UHD 770 took 13 ms.
- **psnr**: one work-item per pixel, each doing a device-scope 64-bit atomic
  add into one accumulator per plane. The B580's compiler combines these; the
  UHD 770 did not (10.7 ms for luma).

### 3. What was changed, and what each change bought

Measured the same way on the new build (3840x2160, ms per frame):

| Phase | psnr_hvs B580 | psnr_hvs UHD 770 | motion_v2 B580 | motion_v2 UHD 770 | psnr B580 | psnr UHD 770 |
| --- | --- | --- | --- | --- | --- | --- |
| Host copy / conversion before upload | 0 | 0 | 0 | 0 | 0 | 0 |
| Shared chroma upload, host time (once per frame, all twins) | 0.81 | 1.12 | — | — | 0.81 | 1.12 |
| Shared chroma DMA, device time | 0.61 | 0.24 | — | — | 0.61 | 0.24 |
| Private copies, device time | — | — | 0.03 (device-to-device) | 0.20 | — | — |
| Kernels, device time | 1.31 (1 launch) | 49.44 | 0.36 (variant, not shipped) | 6.31 (variant, not shipped) | 0.14 (luma 0.09, chroma 0.05) | 3.35 (luma 2.23, chroma 1.12) |
| Host wait in `collect()` | 0.08 | 0.82 | 0.05 | 1.07 | 0.02 | 0.73 |

All three twins now read the planes the SYCL state already holds. Luma comes
from the shared frame; Cb and Cr come from new opt-in shared chroma planes
(ADR-1369) that the first chroma-reading twin of a frame uploads and every
other twin reuses. Only the result leaves the device: psnr_hvs reads back its
block scores, psnr and motion_v2 one 64-bit sum per plane.

**psnr_hvs kernel.** Two work-items per 8x8 block, one per image. Each stages
its 64 raw samples in local memory, takes the variance ratio, runs the integer
DCT in place (column transforms written back as columns, then row transforms:
the column pass leaves the transpose of the CPU's scratch `z`, so the row pass
reads what the CPU's second pass reads) and sums its masking energy. The pair
exchanges ratio and energy through the sub-group, and the reference work-item
scores the block. Every float expression is the previous kernel's, in the same
order, so each block score is bit-identical. A local-memory pad word per
work-item puts the sub-group's lanes on different banks. On the B580 the
kernel went from 6.26 to 1.31 ms; half of what is left is the byte gathers of
the samples. The Xe2 compile crash of `T-SYCL-PSNR-HVS-B580-SIGSEGV` does not
come back: the AOT build for `bmg-g21` succeeds, including a scratch variant
with `reqd_sub_group_size(32)`.

On the UHD 770 the kernel went from 104 to 49 ms, and that is not enough to be
useful there. Ablations of the new kernel on the UHD 770: without the error
loop 38.8 ms, without the DCT 42.3, without the variance ratio 41.8, without the
global loads 44.3. No single phase dominates; the per-block serial chains run
at SIMD8 (forcing SIMD16 took 84.6 ms and SIMD32 145.6, so the compiler's own
choice is best) with at most a few threads per EU. `#pragma unroll 1` on the
8x8 loops made it slower (96 ms). Work-group sizes 32, 64 and 128 are the same.
Getting further on Xe-LP needs a different per-block float order, which the
bit-identity requirement rules out for now.

**motion_v2 kernel (measured, not shipped).** On rebase this kernel gave way
to [ADR-1371](../adr/1371-sycl-motion-diff-first-pipeline.md), which moved
`motion_sycl` and `motion_v2_sycl` onto one SAD pipeline
(`integer_motion_pipeline_sycl.cpp`) that still computes the vertical taps
per output pixel (in 32 bits for bpc ≤ 15). motion_v2 keeps only the host
change: it runs that pipeline on the shared luma and uses its `cur_copy` for
the ping-pong. Against master `b20472dcb` (ADR-1371) that is 6.1 → 5.4 ms per
4K frame over 100 frames on the B580 and unchanged on the UHD 770
(14.7 → 15.1, within noise, the pipeline kernel dominates). The variant, for
the pipeline follow-up: the vertical pass runs once per tile column into local
memory (8 x 36 sums per 32 x 8 tile instead of 5 per output pixel). Integer
arithmetic stays exact in 32 bits: the vertical sum is at most
2^16 · (2^bpc − 1) + 2^(bpc − 1) < 2^31 for bpc ≤ 15 (16-bit input keeps the
64-bit sum), and the horizontal sum, which needs 33 bits, is split into its
three products (each below 2^31) as
(h + 2^15) >> 16 = Σ (p_i >> 16) + ((Σ (p_i & 0xFFFF) + 2^15) >> 16). The
work-group sums |blurred| ≤ 65535 in 32 bits; only the frame total is 64-bit.
13.2 → 6.3 ms on the UHD 770, 0.58 → 0.36 on the B580.

**psnr kernel.** Each work-item sums 16 pixels one grid apart, the work-group
reduces, and one atomic per group adds the total. Squares are 32-bit
(|diff| ≤ 65535 squared fits 32 unsigned bits) and a work-item's 16 of them
stay 32-bit for bpc ≤ 12. 10.7 → 2.2 ms (luma) on the UHD 770.

### 4. How the chroma gets to the device

The first version copied each chroma plane straight from the picture: one
memcpy when its rows were packed, one `ext_oneapi_memcpy2d` when the stride
was wider than the row. Both are copies from pageable memory. At 3840x2160 the
chroma rows are packed and the driver staged them on the calling thread
(1.13 ms of host time on the B580, 3.19 on the UHD 770). At 576x324 the 288-byte
chroma rows sit in a 320-byte stride, and the 2-D copy from pageable memory
took 5.1 ms per frame on the B580 and 168 ms on the UHD 770 — the whole
576x324 psnr run went from 1.9 to 254 ms per frame on the UHD 770 in the first
timing pass. The upload now packs each plane into one of four pinned staging
buffers (host USM, allocated with the chroma planes) and issues one DMA per
plane from there: 0.06 to 0.07 ms of host time at 576x324 and 0.81 / 1.12 ms at
3840x2160 on the B580 / UHD 770. The next frame's packing waits on the last
copy out of the staging buffers; that copy has always finished, because every
`vmaf_read_pictures()` waits for the frame's last upload before it returns.

### 5. Host costs that remain

- The shared-frame luma upload's host time, 2.2 to 3.0 ms per 4K frame on the
  B580: the CLI's picture pool is pageable, so the driver copies each plane
  into a staging buffer on the calling thread, and a pitched luma plane goes up
  as one copy command per row. Allocating the pool in host USM
  (`sycl::malloc_host`) when a SYCL state is attached would make the luma and
  chroma uploads asynchronous DMA from the picture itself, without the chroma
  packing copy. That is a change to the generic picture pool
  (`picture_pool.c`, `libvmaf.c` `prepare_picture_pool`), which the CLI
  read-ahead work also touches, so it is left as a follow-up
  (`T-SYCL-PAGEABLE-UPLOAD-HOST-STAGING-2026-09-29`).
- A slot fence: the upload into a double-buffer slot now waits on the device
  for the kernels that last read that slot (markers from
  `ext_oneapi_get_last_event()` on the primary and combined queues, a copy-queue
  barrier only when one is still running). 0.07 to 0.1 ms of host time per
  frame. Before, nothing ordered an upload after the readers of the slot it
  overwrote: the next frame's `collect()` happened to wait for them, except
  for an extractor that skipped frames (`n_subsample`), whose kernels could
  still be reading when the upload two frames later overwrote the slot. Not
  reproduced; found by reading the code while moving psnr_hvs onto the shared
  slots.

### 6. Parity

Every per-frame score of the three twins is bit-identical to master at
`--precision max` on both devices: BBB 3840x2160 (22 frames), the 576x324
pair (48 frames) in 8, 10 and 12 bits and 4:2:2 10-bit, and the 480x270
10-bit pair — 120 of 120 per-metric comparisons (2 devices x 6 fixtures x 3
twins) with a maximum difference of 0. psnr_hvs therefore keeps its distance from the CPU exactly
(7.63e-4 dB combined at 4K, 8.03e-5 at 576x324), inside the ADR-1361 gate
(3.34e-3 and 5e-4). psnr and motion_v2 equal the CPU. `test_sycl_shared_planes`
runs the three twins in one context over four frames with chroma that changes
every frame, at 250x138 (pitched chroma rows), against the CPU; with the chroma
upload broken to "first frame only" it fails on frame 1 (`psnr_cb` 60 against
7.77).

### 7. A latent bit-depth defect

The old psnr_hvs host conversion scaled only 10-, 12- and 16-bit samples, and
`sample_to_int()` then multiplied every depth other than 8 and 10 by 16. For
9- and 11-bit input the kernel therefore saw 16 times the raw sample, where
`calc_psnrhvs()` uses the raw sample. The new kernel reads raw samples at every
depth, so 9- and 11-bit input now scores like the CPU; 8, 10 and 12 bits are
unchanged. Measured through the C API (the CLI accepts 8, 10, 12 and 16 bits
only) on 64x48 4:2:0 on the B580: 9 bits CPU 22.470312 dB, master's twin
-1.568466, new twin 22.470312; 11 bits CPU 33.972263, master 10.431668, new
33.972261. `test_psnr_hvs_odd_depth_parity` in `test_sycl_psnr_hvs_parity`
pins it.

The same probe found that a `VmafSyclState` reused by a second context with a
different frame size keeps the first size's shared frame: `psnr_sycl` scored
psnr_y 11.650906 against the CPU's 11.675598 for a 128x96 context after a 64x48
one on master. Twins that read chroma now fail such a context at init; luma
twins still mis-score it (`T-SYCL-SHARED-FRAME-STICKY-GEOMETRY-2026-09-29`).

## CUDA and HIP twins (read-only)

| Twin | Same overhead | Where |
| --- | --- | --- |
| `psnr_hvs_cuda` | Device picture → host (`cuMemcpy2DAsync` + `cuStreamSynchronize`) → host float conversion → host → device, every frame and plane; thread-0-serial kernel, one launch per plane | `integer_psnr_hvs_cuda.c` `upload_frame` (`issue_d2h_plane`, `convert_plane`, `issue_h2d_plane`), `integer_psnr_hvs/psnr_hvs_score.cu` |
| `psnr_hvs_hip` | Host float conversion of the host picture and float H2D per plane; same kernel shape | `integer_psnr_hvs_hip.c` conversion loops and `launch_psnr_hvs`, `psnr_hvs_score.hip` |
| `motion_v2_cuda`, `motion_v2_hip` | 25 64-bit multiply-adds per pixel (vertical pass recomputed per output); HIP also uploads its own copy of the reference luma (host-picture backend) | `integer_motion_v2/motion_v2_score.cu`, `motion_v2_score.hip`, `integer_motion_v2_hip.c` `mv2_hip_launch` |
| `psnr_hip` | Each HIP twin uploads its own planes (`vmaf_hip_picture_upload`); no device frame shared between twins | `integer_psnr_hip.c` `submit_fex_hip` |

`psnr_cuda` already reads the device picture and reduces per warp.

## Open questions

- The pinned picture pool (section 4).
- Whether a UHD 770 path for psnr_hvs is worth a per-block float order that
  differs from the CPU's (it would stay inside the ADR-1361 gate, but no longer
  bit-identical to the current twin).
