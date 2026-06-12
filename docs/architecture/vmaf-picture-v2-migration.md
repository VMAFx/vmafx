<!-- markdownlint-disable MD060 -->
# `VmafPicture` v2 — consumer migration guide

> **Status:** Implemented (cycle N+1). v2 is declared in
> `core/include/libvmaf/picture_v2.h` and linked into `libvmaf.so`
> via `core/src/picture_v2.c`. See
> [ADR-0928](../adr/0928-vmaf-picture-v2-explicit-backend-state.md)
> for the decision record and the four-cycle migration plan.

## Why v2 exists

The v1 struct (`core/include/libvmaf/picture.h`) carries a single
`void *priv` that every backend (CPU, CUDA, SYCL, HIP, Metal) overlays
its own shadow record on top of. There is no way for a downstream
consumer to inspect a `VmafPicture` and discover which backend owns
its plane data — misrouting a CUDA-backed picture into a SYCL
extractor is silent UB rather than an explicit `-EINVAL`.

v2 replaces the `void *priv` overlay pattern with two explicit fields:

```c
typedef struct VmafPicture2 {
    /* ... v1 prefix (pix_fmt, bpc, w, h, stride, data, ref) ... */
    void *priv;
    VmafBackendHandle backend;     /* discriminator */
    uintptr_t backend_handle;      /* CUstream / hipStream_t / queue cookie / ... */
    uintptr_t _reserved[4];        /* additive-growth tail */
} VmafPicture2;
```

`backend` is an enum (`VMAF_BACKEND_HANDLE_NONE | _CUDA | _SYCL | _HIP |
_METAL | _VULKAN`). `backend_handle` carries the typed stream/queue
pointer for that backend, narrowed at the consumer side via a documented
cast (`CUstream s = (CUstream)pic.backend_handle;`).

## Timeline (4 cycles)

| Cycle | Window | What ships | SONAME | v1 status |
|---|---|---|---|---|
| **N** | 2026-05-31 | `picture_v2.h` declared; ADR-0928; this doc; changelog fragment | 3 | live default |
| **N+1** (this RC) | 2026-06-13 | `picture_v2.c` implemented; header installed; unit tests; `picture_v2.h` wired into meson | 3 (additive) | live default |
| **N+2** | ≈ 6 months | In-tree backends + tools + `ffmpeg-patches/0002-0006` switched to v2 | 3 | callable but unused in-tree |
| **N+3** | ≈ 12 months (target VMAFX v4.0.0) | v1 entry points removed; `typedef VmafPicture2 VmafPicture;` | **3 → 4** | removed |

## Migration recipes

### FFmpeg filter (`ffmpeg-patches/0002-0006`)

**Before (v1):**

```c
VmafPicture pic;
vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 1920, 1080);
copy_picture_data(frame, &pic, 8);
vmaf_read_pictures(ctx, &pic, NULL, frame_index);
vmaf_picture_unref(&pic);
```

**After (v2):**

```c
VmafPicture2 pic;
vmaf_picture2_alloc(&pic, VMAF_PIX_FMT_YUV420P, 8, 1920, 1080);
/* pic.backend == VMAF_BACKEND_HANDLE_NONE — CPU picture */
copy_picture_data_v2(frame, &pic, 8);
vmaf_read_pictures2(ctx, &pic, NULL, frame_index);
vmaf_picture2_unref(&pic);
```

The hwaccel-import filters (libvmaf_sycl, libvmaf_vulkan,
libvmaf_metal) set `pic.backend` and `pic.backend_handle` directly
instead of stashing the stream inside `pic.priv`.

**Coordination rule:** Per CLAUDE.md §12 r14, the cycle-N+1 PR that
implements `vmaf_picture2_alloc` must update the FFmpeg patch stack in
the same PR so the next `git am --3way` against `n8.1` does not break.
For the cycle-N+2 switch, each of patches 0002–0006 ships an updated
copy that consumes v2 directly.

### Rust safe binding (modernization #11)

The Rust wrapper targets v2 from day one — it never wraps v1. The
binding maps `VmafBackendHandle` to a Rust enum and exposes the
stream handle as a typed associated type:

```rust
pub enum Backend {
    None,
    Cuda(CudaStream),
    Sycl(SyclQueue),
    Hip(HipStream),
    Metal(MetalCommandQueue),
}

pub struct Picture {
    inner: ffi::VmafPicture2,
}

impl Picture {
    pub fn backend(&self) -> Backend { /* match on self.inner.backend */ }
}
```

Until cycle N+1 ships the implementation, the Rust binding can build
against the header but every call returns `-ENOSYS` (the design
header declares the entry points but they are not yet linked).

### MCP server (`mcp-server/vmaf-mcp/`)

The MCP tool surface continues to accept v1 pictures in cycle N+1. In
cycle N+2 the JSON-RPC schema gains an optional `backend` field on
picture descriptors; absent → `none` (CPU). The MCP server uses
`vmaf_picture_v1_to_v2` internally to promote inbound v1 pictures.

### Controller / node (ADR-0709, Phase 4b)

The controller-to-node gRPC protocol assumes v2's typed backend
handle from inception — `vmafx-node` workers report
`backend: VMAF_BACKEND_HANDLE_CUDA` (etc.) explicitly so the
controller can schedule cross-node parity sweeps onto matched silicon.

### Downstream Python wheels

The Python harness (`compat/python-vmaf/`) wraps v1 today. Cycle N+1
adds a v2 wrapper; cycle N+2 deprecates the v1 wrapper. Pre-built
wheels distributed via PyPI follow the SONAME bump in cycle N+3.

## What does NOT change

- **Plane data layout** (`data[3]`, `stride[3]`) is identical between
  v1 and v2. Hot loops that index `pic.data[plane][y * stride + x]`
  read the same bytes either way.
- **`VmafRef` refcounting** behaves the same. v2's `ref` slot is
  the same `VmafRef *` as v1.
- **CPU-only consumers** see no behaviour change beyond the type
  rename — `backend == VMAF_BACKEND_HANDLE_NONE`, `backend_handle == 0`.
- **Score values** — v1 and v2 score paths share the same extractor
  implementations; numerical equivalence is preserved.

## Cross-references

- [ADR-0928](../adr/0928-vmaf-picture-v2-explicit-backend-state.md)
  — decision record + alternatives + consequences.
- [ADR-0186](../adr/0186-vulkan-image-import-impl.md) — first
  hwaccel-import case that motivated typed backend handles.
- [ADR-0700](../adr/0700-core-directory-rename.md) — VMAFX
  rebrand directory move; aligned with v2 lifecycle.
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md)
  — controller/node protocol that consumes v2.
- CLAUDE.md §12 r14 — FFmpeg-patches lockstep rule.
- `core/include/libvmaf/picture.h` — v1 header (frozen).
- `core/include/libvmaf/picture_v2.h` — v2 design header.
