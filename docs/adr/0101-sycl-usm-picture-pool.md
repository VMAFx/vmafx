<!-- markdownlint-disable MD060 -->
# ADR-0101: SYCL USM-backed picture pre-allocation pool

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: `sycl`, `gpu`, `picture-api`, `memory`

## Context

`vmaf_sycl_preallocate_pictures()` and `vmaf_sycl_picture_fetch()` shipped in
an earlier SYCL backend scaffolding PR (#25) as no-op stubs:

```c
int vmaf_sycl_preallocate_pictures(VmafContext *vmaf, VmafSyclPictureConfiguration cfg)
{
    // SYCL extractors handle picture upload internally,
    // so preallocation is not strictly needed.
    (void)vmaf; (void)cfg; return 0;
}

int vmaf_sycl_picture_fetch(VmafContext *vmaf, VmafPicture *pic)
{
    return vmaf_picture_alloc(pic, vmaf->pic_params.pix_fmt, ...);
}
```

The stubs were documented as "not strictly needed" because the primary SYCL
upload path (`vmaf_sycl_shared_frame_upload` in `src/sycl/common.cpp`) copies
from host `VmafPicture.data[0]` into double-buffered shared device USM buffers
once per frame. The fetch stub simply delegated to `vmaf_picture_alloc`, which
returns a host-backed picture.

Issue #26 tracks this as a correctness gap: the public API implies
"DEVICE" and "HOST" pre-allocation modes return USM-backed pictures, but in
fact *every* caller receives an ordinary host picture regardless of
`pic_prealloc_method`. Callers that expect a device pointer (e.g., for
zero-copy interop with a decoder that writes straight to SYCL USM) have to
work around the silent fallback. The CUDA backend already ships a full
implementation via `vmaf_cuda_preallocate_pictures` + `VmafRingBuffer`
(`src/cuda/ring_buffer.c`); SYCL had parity only on paper.

## Decision

Implement a USM-backed picture pool for SYCL — `VmafSyclPicturePool` in
`core/src/sycl/picture_sycl.{h,cpp}` — and wire
`vmaf_sycl_preallocate_pictures()` / `vmaf_sycl_picture_fetch()` /
`vmaf_close()` to use it.

Key choices:

- **Scope**: Y-plane only. VMAF operates on luma; U/V planes are not read by
  any SYCL extractor. Keeping scope identical to the CUDA pool avoids diverging
  the two backends' contracts.
- **Pool depth**: 2 pictures. Matches the double-buffered shared-frame upload
  (`cur_upload` / `cur_compute` in `VmafSyclState`), so frame N+1 can start
  filling pool slot 1 while frame N is still being consumed from slot 0.
- **Allocator selection**: `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE` →
  `sycl::malloc_device`; `VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_HOST` →
  `sycl::malloc_host`. `NONE` remains a true no-op (no pool created;
  `vmaf_sycl_picture_fetch` falls back to `vmaf_picture_alloc`).
- **Refcount semantics**: Each pool entry is initialised with a VMAF ref (=1)
  owned by the pool. `vmaf_sycl_picture_pool_fetch` calls `vmaf_picture_ref`
  which increments to 2 — the caller owns the outer ref and releases it via
  `vmaf_picture_unref` (→ back to 1, pool still holds). `vmaf_close` calls
  `vmaf_sycl_picture_pool_close` which walks the pool and releases each
  entry's buffers + priv + ref.
- **buf_type tag**: Every pool picture gets
  `VmafPicturePrivate.buf_type = VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE`
  (regardless of DEVICE vs HOST USM method) so
  `validate_pic_params` can enforce consistent backing across ref/dist pairs.

The primary upload path (`vmaf_sycl_shared_frame_upload`) is unchanged — SYCL
copy-queue memcpy is allocator-agnostic, so a USM source works identically to a
host source (transparent D2D or H2D depending on allocator kind).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the no-op stubs | Zero code change; no blast radius | Silent API lie: DEVICE/HOST modes return a host picture. Breaks zero-copy decoder interop. Issue #26 stays open indefinitely. | Rejected: the public API contract requires real USM buffers on these modes. |
| Unify with CUDA `VmafRingBuffer` | Single implementation, single refcount model | Would require shimming cuStream semantics onto a sycl::queue; ring_buffer.c pulls in CUDA headers unconditionally. High churn for no functional gain. | Rejected: blast radius touches CUDA code paths that are already stable. |
| Extend generic `picture_pool.c` (the reusable byte-pool from ADR-0029) | One pool for all backends | `picture_pool.c` is host-byte oriented; adding USM support means injecting a backend-dispatch layer into code that currently has zero GPU awareness. Leaks SYCL types into a generic path. | Rejected: keeps the generic pool focused on host byte buffers. |

## Consequences

- **Positive**:
  - `VmafSyclPictureConfiguration.pic_prealloc_method` now behaves as its
    public doc-comment promises for all three enum values.
  - Zero-copy decoder interop (VPL → Level Zero → SYCL USM) has a landing
    spot: the decoder can write directly into `pic.data[0]` on a pool
    picture without a host round-trip.
  - Pool lifetime is bounded by `vmaf_close()`, matching CUDA behaviour.
- **Negative**:
  - Adds ~180 lines of C++ (`picture_sycl.cpp`) plus a small API surface
    (`VmafSyclPoolMethod`, three `vmaf_sycl_picture_pool_*` functions) in the
    internal header.
  - Two new USM allocations per `vmaf_sycl_preallocate_pictures` call — minor
    GPU memory baseline increase (e.g., 2 × 1920×1080 × 1 B ≈ 4 MB on
    default 8-bit 1080p runs).
- **Neutral / follow-ups**:
  - New test: `core/test/test_sycl_pic_preallocation.c` covers NONE /
    DEVICE / HOST / no-state / double-preallocate / refcount-cycle paths.
  - `docs/api/gpu.md` updated — removes the "SYCL preallocation is a
    no-op" callout introduced in PR #25.
  - PR #25 tracked Issue #26 as "scaffolded but not implemented"; this ADR
    closes that gap.

## References

- Issue #26 — "SYCL: `vmaf_sycl_preallocate_pictures` is a no-op stub"
- PR #25 — original SYCL preallocation scaffolding (public API surface)
- `core/src/cuda/ring_buffer.c` — CUDA reference implementation
- `core/src/sycl/common.cpp` (`vmaf_sycl_shared_frame_upload`) — confirms
  SYCL copy-queue memcpy is allocator-agnostic
- Source: `req` — user selected "Option 1 — implement USM properly" and "Yes —
  full ADR under docs/adr/" in session 90ad1e1d AskUserQuestion popup.
