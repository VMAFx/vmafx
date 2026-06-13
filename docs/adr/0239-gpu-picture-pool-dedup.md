# ADR-0239: Backend-agnostic GPU picture pool (`gpu_picture_pool.{h,c}`)

- **Status**: Accepted
- **Date**: 2026-05-02
- **Deciders**: Lusoris
- **Tags**: refactor, gpu, cuda, sycl, vulkan, dedup, fork-local

## Context

The 2026-05-02 GPU dedup audit (`docs/research/0046-gpu-dedup-audit.md`)
identified the picture-pool lifecycle as the single most duplicated
GPU surface in the fork:

- `core/src/cuda/ring_buffer.c` (143 LOC, callback-based round-robin
  pool — original Netflix code).
- `core/src/sycl/picture_sycl.cpp` (~80 LOC of pool init / fetch /
  close, hand-rolled C++ with `std::mutex` + inline alloc).
- `core/src/vulkan/picture_vulkan_pool.c` (~180 LOC, freshly added
  in PR #264, deliberately mirrors the SYCL shape line-for-line).

The shape is identical across the three: validate config → calloc the
slot array → loop alloc-callback with unwind on failure → mutex-locked
round-robin fetch → mutex-locked close that runs the free-callback per
slot. Only the **callback bodies** vary — `cudaMalloc` vs
`sycl::malloc_device` vs `vmaCreateBuffer`. The CUDA pool already
expressed this via the `VmafRingBufferConfig.alloc_picture_callback`
interface; SYCL and Vulkan re-implemented the lifecycle around their
own backend-specific cookies.

User's 2026-05-02 framing (popup answer): "all three sequenced — pool
first, headers second, kernels third". This ADR is the "pool first"
PR.

## Decision

We will promote `core/src/cuda/ring_buffer.{c,h}` out of the
`cuda/` subdirectory into `core/src/gpu_picture_pool.{c,h}` and
rename its symbols to remove the CUDA-specific naming (Netflix's
`VmafRingBuffer` was always agnostic in shape — only the directory
and symbol names implied otherwise):

| Before (CUDA-specific name) | After (backend-agnostic) |
| --- | --- |
| `VmafRingBuffer` | `VmafGpuPicturePool` |
| `VmafRingBufferConfig` | `VmafGpuPicturePoolConfig` |
| `vmaf_ring_buffer_init` | `vmaf_gpu_picture_pool_init` |
| `vmaf_ring_buffer_close` | `vmaf_gpu_picture_pool_close` |
| `vmaf_ring_buffer_fetch_next_picture` | `vmaf_gpu_picture_pool_fetch` |
| `cuda/ring_buffer.{c,h}` | `gpu_picture_pool.{c,h}` |
| `test_ring_buffer.c` | `test_gpu_picture_pool.c` |

CUDA call sites in `libvmaf.c` migrate mechanically (find/replace).
SYCL's `vmaf_sycl_picture_pool_*` keeps its public-internal API (the
caller side in `libvmaf.c` stays the same) but the implementation
delegates to the generic pool — the SYCL wrapper now just owns the
`VmafSyclCookie` storage so the alloc/free callbacks have a stable
pointer for the pool's lifetime. `std::mutex` drops out (the generic
pool uses `pthread_mutex_t`, which works fine in C++ TUs).

Vulkan migration **bundled into this PR** after #264 (Vulkan
picture preallocation, ADR-0238) merged. `picture_vulkan_pool.c`
rewrites as a thin wrapper around the generic pool — same pattern
as the SYCL migration: a wrapper struct owns the per-pool state
that the alloc/free callbacks need, the generic pool owns the
round-robin slots / mutex / unwind. Net structural win: ONE
round-robin implementation across all three backends.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Promote ring_buffer.{c,h} to gpu_picture_pool.{c,h} (chosen)** | The CUDA pool was *already* the right shape; promotion is mechanical; SYCL becomes a thin wrapper; Vulkan follow-up is symmetric | One file move + ~20-call-site rename in libvmaf.c | Picked: the audit confirmed the shape was already generic; only the location and names implied otherwise |
| Add a thin generic helper alongside the existing three pools | Smaller blast radius — no rename | Doesn't actually dedup; ships a fourth pool implementation | Rejected: the whole point is removing the SYCL/Vulkan bespoke pools |
| Keep the CUDA name, alias under generic name | Backwards-compat with any external users of `VmafRingBuffer` | The fork has no external users (internal-only header, no public C-API exposure); aliases would obscure the rename | Rejected: no external consumers, no need to carry two names |
| Wait for Vulkan PR #264 to merge first | Cleaner sequencing — one PR migrates all three | Adds a serial dependency between two unrelated PRs; #264's CI is already in flight | Rejected: this PR ships independently against current master; Vulkan follow-up handles its migration after #264 lands |

## Consequences

- **Positive**:
  - Single source of truth for the GPU picture-pool lifecycle. New
    GPU backends (HIP scaffold under T7-10, future Metal / DirectML)
    use `vmaf_gpu_picture_pool_*` directly without re-implementing
    the round-robin + mutex pattern.
  - SYCL pool drops ~70 LOC of duplicated init/fetch/close logic
    and `std::mutex` plumbing.
  - Vulkan migration (follow-up PR) drops another ~100 LOC.
  - The CUDA `Netflix#1300` mutex-destroy-order fix (ADR-0157)
    travels with the file — preserved verbatim, just relocated.
- **Negative**:
  - File-rename churn affects rebases pulling upstream changes that
    touch `core/src/cuda/ring_buffer.c`. Unlikely (Netflix hasn't
    touched this file since `cb1d49c6`); rebase-notes 0100 documents
    the rename.
  - CUDA's `VmafRingBuffer*` was technically a public-internal name
    (referenced from `libvmaf.c`). The rename is fork-local; nothing
    outside `libvmaf/` consumed those symbols.
- **Neutral / follow-ups**:
  - **PR3 of the dedup sequence**: GPU public header codegen
    (`libvmaf_{cuda,sycl,vulkan,hip}.h` from one template).
  - **PR4 of the dedup sequence**: GPU feature kernel host-glue
    extract (10+ files per backend share state/init/close
    boilerplate).
  - **Vulkan migration follow-up**: rewrite `picture_vulkan_pool.c`
    as a thin wrapper around the generic pool after PR #264 lands.

## References

- Source: `req` 2026-05-02 — popup answer "all three sequenced —
  pool first, headers second, kernels third".
- 2026-05-02 GPU dedup audit (in-conversation, agent-produced).
- [ADR-0157](0157-cuda-preallocation-leak-netflix-1300.md) — the
  `Netflix#1300` mutex-destroy-order fix that travels with the file.
- [ADR-0238](0238-vulkan-picture-preallocation.md) — adds
  `picture_vulkan_pool.c`; this ADR's Vulkan-migration follow-up is
  gated on #264 landing.
- [ADR-0250](0250-tiny-ai-extractor-template.md) — tiny-AI extractor
  template precedent (PR #251), refactor pattern model.

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `core/src/gpu_picture_pool.{c,h}` — present.
- `core/src/cuda/ring_buffer.*` — removed (verified by `ls`
  returning no match), confirming the rename + promotion happened.
- The promotion landed alongside the rename matrix declared in the
  Decision section (`VmafRingBuffer` → `VmafGpuPicturePool`, etc.).
- Verification command:
  `ls core/src/gpu_picture_pool.{c,h};
  ls core/src/cuda/ring_buffer.* 2>&1 | grep -i 'no match'`.
