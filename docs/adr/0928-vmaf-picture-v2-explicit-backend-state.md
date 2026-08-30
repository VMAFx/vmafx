<!-- markdownlint-disable MD060 -->
# ADR-0928: VmafPicture v2 — explicit per-backend GPU state

- **Status**: Proposed
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude Code
- **Tags**: api, abi, gpu, cuda, sycl, hip, metal, ffmpeg, rust, fork-local, vmafx-rebrand

## Context

`VmafPicture` (`core/include/libvmaf/picture.h`) is the central
producer/consumer struct for every backend on the fork — CPU plus
four GPU backends (CUDA, SYCL, HIP, Metal). Today the only
backend-state hook on the struct is a single `void *priv`:

```c
typedef struct VmafPicture {
    enum VmafPixelFormat pix_fmt;
    unsigned bpc;
    unsigned w[3], h[3];
    ptrdiff_t stride[3];
    void *data[3];
    VmafRef *ref;
    void *priv;
} VmafPicture;
```

`priv` is treated as a `VmafPicturePrivate *` whose layout the *core*
owns (`core/src/picture.c`), but every backend overlays its own
shadow record on top:

- `core/src/cuda/picture_cuda.c` reads/writes its CUDA-stream + event
  pair through `priv`.
- `core/src/sycl/picture_sycl.cpp` stores the USM allocation + queue
  cookie in `priv`.
- `core/src/hip/picture_hip.{c,h}` (scaffold) plans the same shape
  for HIP streams.
- `core/src/picture_pool.c` overlays a `PooledPicturePriv` on top of
  the same field.

This has three concrete pain points:

1. **No safe round-trip across backends.** A caller (FFmpeg filter,
   downstream Rust binding, MCP client) cannot inspect a
   `VmafPicture` and discover which backend produced it. Mis-routing
   a CUDA-backed picture into a SYCL extractor is a silent
   `priv`-cast UB rather than an explicit `-EINVAL`.
2. **No typed handle for hwaccel zero-copy import.** Vulkan-image
   import (ADR-0186) and HIP stream-pinned uploads need a stable
   "this is a device handle" carrier; the current path forces every
   backend to invent its own field inside `priv`.
3. **`ffmpeg-patches/0002-0006` already inspect `VmafPicture` by
   address.** The patches read `pic->data[]`, `pic->stride[]`, and
   in two places assume `priv` was set by a specific backend. Any
   rewrite of `VmafPicture` must coordinate the FFmpeg patch stack
   in the same release window or the next `git am --3way` over
   `n8.1` breaks.

A future ABI break for the VMAFX rebrand (ADRs 0700, 0709, 0712)
is also the natural moment to fix the `void *priv` carrier, because
soname-bumping libvmaf.so once is cheaper than twice.

## Decision

We will introduce **`VmafPicture2`** in a new public header
`core/include/libvmaf/picture_v2.h`. The v2 struct carries an
explicit `enum VmafBackendHandle backend` discriminator and a typed
`uintptr_t backend_handle`. v1 (`VmafPicture`) is preserved bit-for-bit
for the deprecation window — both APIs ship side-by-side for one
minor release. The `.so` SONAME bump from `libvmaf.so.3` →
`libvmaf.so.4` lands in the *next* major release (target: v4.0.0),
after the dual-API window closes and v1 is removed.

This PR ships only the design header, migration plan, ADR, and
documentation. No `core/src/*` changes. No `meson.build` changes.
No SONAME bump. Header is declared, not yet built into libvmaf.so.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Keep `void *priv`, document harder** | Zero ABI churn, no soname bump | Doesn't solve any of the three pain points; backend mis-routing remains silent UB; Rust bindings still need an out-of-band lookup table | Loses the cross-backend round-trip property the rebrand depends on |
| **Add `backend` enum to v1 in-place via end-of-struct growth** | No new header, no v1/v2 split | Header rule (`AGENTS.md`: "Configuration / Picture-configuration structs grow at the end") allows the addition, but doesn't solve the `priv` overlay confusion — backends would still cast `priv` and ignore the new discriminator | Half-measure; ships the cost of an ABI growth without the round-trip win |
| **Replace v1 in place + bump soname now** | Single ABI break, smallest long-term surface | Breaks every consumer in the same release: FFmpeg patches 0002–0006, MCP server, Rust binding (mod #11), every downstream Python wheel. No transition path for ffmpeg-patches reviewers | Violates CLAUDE.md §12 r14 (FFmpeg patches must update in lockstep), and gives downstream zero migration window |
| **`VmafPicture2` with dual-API window (chosen)** | Cross-backend round-trip explicit; 12-month deprecation; FFmpeg patches migrate one release at a time; Rust bindings get a typed handle without the v1 `priv` cast | Two structs in tree for one cycle; converter shim required; one extra header to maintain | Smallest blast radius for the rebrand's most-touched struct |
| **Opaque accessor API only — no public struct** | Hides all layout from callers; perfect ABI flexibility | Every consumer must call `vmaf_picture_get_data(pic, plane)` instead of `pic->data[plane]`. ffmpeg-patches 0002–0006 would each grow ~30 lines of accessor calls per hot loop | Performance + churn cost outweighs the flexibility; the v2 struct stays inspectable for ABI v4 |

## Consequences

- **Positive**:
  - Cross-backend round-trip becomes a typed runtime check instead
    of `void *` UB. Misroute → explicit `-EINVAL`.
  - Rust safe binding (modernization #11) can wrap
    `VmafPicture2` with a typed `Backend` enum directly; no
    out-of-band lookup table.
  - HIP / Metal / future-backend stream handles plug into the same
    `backend_handle` slot — no new field per backend.
  - Vulkan-image-import (ADR-0186) gets a typed home for the
    `VkImage` handle in v2 rather than yet-another-`priv`-overlay.
  - SONAME bump is *scheduled* (v4.0.0) not *forced* — gives
    distros a known migration window.

- **Negative**:
  - One extra header to maintain (`picture_v2.h`) for the
    deprecation window (~12 months).
  - Two converter functions in `core/src/picture.c` once v2
    lands (`vmaf_picture_v1_to_v2` / `_v2_to_v1`).
  - FFmpeg patches 0002–0006 will each need a v1→v2 follow-up PR
    in the cycle where v2 becomes the default — coordinated via
    CLAUDE.md §12 r14.

- **Neutral / follow-ups**:
  1. **Cycle N (this PR)**: ship `picture_v2.h` declared only.
     No `.so` impact. SONAME stays at 3.
  2. **Cycle N+1**: wire `picture_v2.h` into `meson.build`,
     implement converters in `core/src/picture.c`, expose
     `vmaf_picture2_alloc` / `vmaf_picture2_unref` /
     `vmaf_picture2_from_v1` / `vmaf_picture2_to_v1`. Mark v1
     `@deprecated` in Doxygen + emit `__attribute__((deprecated))`
     on `vmaf_picture_alloc`. SONAME stays at 3 (additive).
  3. **Cycle N+2** (≈ 6 months): convert in-tree backends + tools
     peg patches to v2. v1 functions still callable but
     `deprecated`. SONAME stays at 3.
  4. **Cycle N+3** (≈ 12 months from this PR, target v4.0.0):
     remove v1 entry points, drop converters, bump SONAME
     3 → 4. FFmpeg-patches stack reset against v2-only libvmaf.
  5. **Rust safe binding** (modernization #11) targets v2 from
     day one; never wraps v1.
  6. **MCP server**: surface inspection unchanged in cycle N+1
     (v1 callers still work). Switch to v2 in cycle N+2.
  7. **AGENTS.md** in `core/include/libvmaf/` gets an invariant
     note on the v1 freeze (no growth, no field additions) for
     the dual-API window — v1 is a *museum piece* once v2 lands.

## References

- `req`: user direction (2026-05-31, paraphrased): scaffold the
  ADR + header for `VmafPicture v2` with explicit GPU-backend state.
  Do not yet bump SOVERSION; this PR is design + transition plan only.
- ADR-0186 (Vulkan image-import) — first hwaccel-import case that
  needed a typed handle.
- ADR-0700 (`core/` directory rename) — same rebrand window where
  v2 lands.
- ADR-0709 (Phase 4b distributed platform) — controller/node
  protocol assumes v2's typed backend handle for cross-node
  scoring.
- CLAUDE.md §12 r14 — FFmpeg patches update in the same PR as the
  public-API change.
- [`docs/architecture/vmaf-picture-v2-migration.md`](../architecture/vmaf-picture-v2-migration.md)
  — consumer migration recipes.
- Related: SONAME policy historically rev'd only at major
  releases; v3→v4 timing aligns with VMAFX v4.0.0.
