<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1034: Fix SYCL integer_vif rd_stride OOB on odd widths and integer_motion UV queue sync gap

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `sycl`, `correctness`, `gpu`

## Context

Two HIGH-severity correctness bugs were identified in the SYCL feature extractors
during the r6 kernel audit:

**Bug 1 — integer_vif rd_stride OOB on odd widths.**
In both the scalar (SIMD-32) and SIMD-16 variants of the horizontal VIF pass
(`launch_vif_hori_impl`, `launch_vif_fused_impl`), the downsampled row stride
was computed as `rd_stride = e_w / 2` (truncating integer division). For odd
frame widths, this underestimates the required row stride. The last even column
thread (e.g. `gx = e_w - 1 = 4` when `e_w = 5`) computes `rd_x = gx/2 = 2`,
but `rd_stride = 5/2 = 2`, producing a linear index of `rd_y * 2 + 2` which is
one element past the row boundary. The write corrupts either the next row or
adjacent device memory. The allocation in `init_fex_sycl` had the same truncation
error: `rd_size = (w/2) * (h/2)`, which underallocates by one column per row for
odd widths.

**Bug 2 — integer_motion UV queue sync gap.**
When `motion_add_uv=true`, the `submit_fex_sycl` function copies UV plane data
to device using `vmaf_sycl_memcpy_h2d_async()`, which submits to `state->queue`
(the primary queue). However, `vmaf_sycl_graph_submit()` only barriers
`combined_queue` on `state->last_upload_event`, which is updated solely by
Y-plane uploads on `copy_queue`. There is no explicit cross-queue dependency
between the primary queue's UV H2D transfers and the combined compute queue.
UV data may not be visible to compute kernels, producing wrong motion scores
for UV planes.

## Decision

**Bug 1**: Change `rd_stride` to ceiling division `(e_w + 1U) / 2U` in both
kernel variants. Change the allocation in `init_fex_sycl` to use
`((w + 1U) / 2U) * ((h + 1U) / 2U)`. For even widths/heights the values are
identical; for odd dimensions the allocation and stride now match.

**Bug 2**: After all UV H2D copies complete, call `vmaf_sycl_queue_wait(state)`
to flush the primary queue before `vmaf_sycl_graph_submit()`. This ensures UV
device writes are globally visible before any compute kernel on `combined_queue`
reads them. The primary queue is in-order and the wait is bounded to one frame's
worth of UV transfers.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Route UV H2D via `copy_queue`, update `last_upload_event` | GPU-side barrier only, no CPU stall | Requires new public API or breaking `vmaf_sycl_memcpy_h2d_async` signature | Larger change scope; the CPU stall is negligible for UV-size copies |
| Add `ext_oneapi_submit_barrier` on primary-queue event inline | No blocking wait | Requires access to primary-queue event handle from a non-C++ context | `vmaf_sycl_memcpy_h2d_async` returns `int`, not `sycl::event`; would need API change |
| `vmaf_sycl_wait_last_upload` (wait only on last DMA event) | Lighter than queue_wait | Only covers copy_queue; UV copies are on primary queue | Does not solve the problem |

## Consequences

- **Positive**: Eliminates device-memory corruption for odd-width video at SYCL
  VIF scale boundaries. Eliminates wrong UV motion scores when `motion_add_uv=true`.
- **Negative**: `vmaf_sycl_queue_wait` after UV copies adds a small CPU-side
  synchronization stall (bounded by UV plane size, typically < 0.1 ms per frame
  on Arc A380).
- **Neutral / follow-ups**: The routing of UV H2D via `copy_queue` (to enable a
  pure GPU-side barrier) is a follow-up optimization tracked as a future
  enhancement.

## References

- r6 SYCL kernel audit findings (HIGH × 3): integer_vif rd_stride OOB +
  integer_motion UV queue sync gap.
- Related: ADR-0989 (motion_add_uv feature).
- Related: `core/src/sycl/common.cpp` — `vmaf_sycl_graph_submit()` barrier logic.
