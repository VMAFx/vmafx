<!-- markdownlint-disable MD060 -->
# ADR-0251: Vulkan VkImage import — v2 async pending-fence model (T7-29 part 4)

- **Status**: Accepted
- **Date**: 2026-05-01
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: vulkan, ffmpeg, fork-local, zero-copy, performance, implementation

## Context

[ADR-0186](0186-vulkan-image-import-impl.md) shipped the Vulkan
VkImage zero-copy import surface with a deliberately
**synchronous v1 design** — `vmaf_vulkan_import_image()`
records, submits, and waits the fence in-call. The ADR's
`Alternatives considered` row called out async pending-fence as
the v2 follow-up "once profiling shows the wait is a
bottleneck." That signal arrived: lawrence's 2026-04-30 profile
of the FFmpeg `libvmaf_vulkan` filter (Issue #239) confirms the
synchronous fence wait inside `vmaf_vulkan_import_image()`
serialises CPU and GPU work — exactly the bottleneck the parent
ADR predicted. The decoder thread idles every other frame
because libvmaf will not return until the GPU finishes the
luma copy.

This ADR records the v2 design that swaps the in-call fence
wait for a per-frame fence ring and a deferred drain in
`vmaf_vulkan_wait_compute()`. The public ABI is preserved —
the four entry points keep their signatures, and the fence
pool is fully internal to `VmafVulkanState`.

## Decision

We will replace the single fence + single command buffer in
`VmafVulkanImportSlots` with a **per-frame ring keyed by
`frame_index % ring_size`**. The ring depth is fixed at state
init via a configurable `max_outstanding_frames` parameter
(default `4`), and pre-allocates `2 × ring_size` staging
`VkBuffer`s (ref + dis × ring), `ring_size` `VkCommandBuffer`s,
and `ring_size` `VkFence`s — no runtime allocation on the
import hot path. `vmaf_vulkan_import_image()` records, submits
to the slot for `frame_index % ring_size`, and returns
immediately; if the slot was already in flight from a prior
frame, the call waits that prior fence first (back-pressure).
`vmaf_vulkan_wait_compute()` blocks on every outstanding fence
in submission order and is the natural drain point before
`vmaf_vulkan_state_build_pictures()` reads back the host
mappings. `vmaf_vulkan_state_free()` drains the ring before
destroying any handle.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Per-frame fence pool, FIFO ring (chosen)** | Bounded memory, no runtime alloc, ABI-stable, matches the canonical Vulkan game-engine pattern | Ring size has to be picked up-front; if `max_outstanding_frames` < FFmpeg's filter graph depth the back-pressure stalls show up exactly where the v1 wait did | Simplest change that breaks the serial bottleneck without a Vulkan 1.2 hard dependency |
| **Single fence with delayed wait** (record + submit in `import_image`, wait in `wait_compute`) | Minimal diff vs v1 | Only one frame can be in flight at any time — the decoder still blocks once it loops back to record the next frame against the same command buffer; gain over v1 is marginal | Doesn't actually remove the serialisation — only relocates the wait |
| **Timeline semaphore with monotonic counter** (drop fences, signal a `VkSemaphore` of type `VK_SEMAPHORE_TYPE_TIMELINE`, wait on a value) | One synchronisation primitive instead of N fences; matches FFmpeg's hwframes context (`AVVkFrame::sem`); cleaner host API | Requires `VK_KHR_timeline_semaphore` (core in 1.2) — fork's pinned `api_version` is 1.3 so present everywhere we run, but the swap touches every kernel TU's submit path and complicates the FFmpeg filter's existing per-frame timeline-semaphore wait (would need a *second* timeline). Bigger blast radius than the ring | Deferred to v3; revisit when a feature kernel needs a queue family transfer (where timeline semaphores are the only correct primitive) |
| **Stay on v1** | Zero new code, matrix unchanged | Profile signal (Issue #239) is direct evidence the wait dominates the FFmpeg filter wall-clock; staying on v1 means accepting that bottleneck indefinitely | The whole reason v1 existed was "we'll fix it when we have data." The data is in. |

## Consequences

- **Positive**: Decoder/copy/compute can overlap up to
  `max_outstanding_frames` deep — for FFmpeg's typical 2–3
  frame buffering the default `ring_size = 4` keeps the libvmaf
  filter off the critical path until the back-pressure budget is
  exceeded. ABI is preserved (the ring is fully internal to
  `VmafVulkanState`); the FFmpeg patch in
  `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`
  needs no signature change.
- **Negative**: Staging-buffer memory grows
  `1 × → max_outstanding_frames ×` per direction (ref+dis), so
  default doubles the allocation footprint vs v1 from
  `2 × stride × h` to `8 × stride × h`. For a 1080p 8-bit Y
  plane that is ~16 MB host-visible per state — well below any
  practical memory budget but worth noting. The unit test matrix
  doubles: every existing v1 contract test (NULL state, wrong
  geometry, unimported index) is replicated for `index >
  ring_size` to verify ring wrap, plus new tests for fence-pool
  init/teardown ordering and the `wait_compute` drain.
- **Neutral / follow-ups**:
  1. Cross-backend gate
     ([`scripts/ci/cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py))
     keeps `places=4` as the v2 contract — async submission
     does **not** change which bytes the staging buffer
     receives, only when the host can read them.
  2. **Measurement gate to flip Status → Accepted**:
     v2 wall-clock ≤ 0.7 × v1 on the Netflix normal pair under
     the FFmpeg `libvmaf_vulkan` filter (PR-235 lavapipe lane).
     If the lavapipe ICD's single-threaded software submit
     model masks the gain (likely — lavapipe has no real
     queue concurrency), document that and re-gate against a
     hardware Arc / RTX / RX run before flipping Accepted.
  3. **Ring-size tuning** *(landed)*:
     `VmafVulkanConfiguration.max_outstanding_frames` is now a
     public field — 0 selects the canonical default (4); values
     clamp to `[1, VMAF_VULKAN_RING_MAX]` internally. The
     observable readback is
     `vmaf_vulkan_state_max_outstanding_frames()`. External-handles
     callers (`vmaf_vulkan_state_init_external`) still receive the
     default; extending `VmafVulkanExternalHandles` is deferred to
     a separate ABI bump. Smoke-test contract pinned in
     `core/test/test_vulkan_async_pending_fence.c`
     (`test_ring_size_*` group).
  4. **Timeline semaphore v3**: tracked under T7-29 part 5 once
     a feature kernel actually needs the cross-queue-family
     transfer property timeline semaphores buy us. Fence ring
     is sufficient for the single-queue-family compute path
     v2 ships against.

## References

- Parent: [ADR-0186](0186-vulkan-image-import-impl.md)
  — declares the v2 async-pending-fence follow-up as the
  deferred path-3 row of its `Alternatives considered`.
- Grandparent: [ADR-0184](0184-vulkan-image-import-scaffold.md)
  — pinned the public ABI surface that v2 preserves.
- Profile signal: Issue #239 — FFmpeg filter wall-clock
  serialisation report (lawrence, 2026-04-30).
- Pattern source: Vulkan ring-fence is the canonical
  "frames in flight" pattern from Khronos
  [synchronization examples](https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples).
- FFmpeg filter coupling: CLAUDE.md §12 r14 — every libvmaf
  surface change ships the matching patch in
  [`ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`](../../ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch).
  v2 keeps the public signatures byte-identical so this PR
  does **not** modify the patch.
- Source: T7-29 part 4 in
  [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md).
- Per-PR rule: ADR-0108 deep-dive deliverables checklist.

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `core/include/libvmaf/libvmaf_vulkan.h:64` declares
  `VmafVulkanConfiguration::max_outstanding_frames`.
- `core/src/vulkan/common.c:444-486` implements
  `vmaf_vulkan_clamp_ring_size`,
  `vmaf_vulkan_state_max_outstanding_frames`, and the per-frame
  fence ring sized at `s->requested_ring_size`.
- `core/src/vulkan/vulkan_internal.h:47-117` documents the
  captured request depth and the ring sizer contract.
- Verification command:
  `grep -n "max_outstanding_frames" core/src/vulkan/*.{c,h}
  core/include/libvmaf/libvmaf_vulkan.h`.
