<!-- markdownlint-disable MD013 -->
# Research-0042: Vulkan async pending-fence model for the VkImage zero-copy import path

- **Date**: 2026-05-01
- **Author**: Lusoris, Claude (Anthropic)
- **Status**: Companion to ADR-0251 (Proposed)
- **Tags**: vulkan, ffmpeg, performance, fork-local

## Problem

[ADR-0186](../adr/0186-vulkan-image-import-impl.md) shipped the
`vmaf_vulkan_import_image()` entry point with a synchronous
in-call fence wait — `vkCmdCopyImageToBuffer` is recorded,
submitted, and `vkWaitForFences` blocks the calling thread
until the GPU finishes the luma copy. This was the explicit
"unblock T7-29 part 3 first, optimise later" choice noted in
the parent ADR's `Alternatives considered`. lawrence's
2026-04-30 profile of the FFmpeg `libvmaf_vulkan` filter
(Issue #239) confirms the predicted bottleneck: the
synchronous fence wait inside `vmaf_vulkan_import_image()`
serialises the decoder thread and the libvmaf compute queue
even though they share no data dependency until
`vmaf_vulkan_state_build_pictures()` reads the host
mappings.

The fix is the canonical Vulkan "frames-in-flight" pattern:
record + submit + return immediately, drain the fence later
when the host actually needs the data. This research digest
records the option-space we walked through to land on a
fixed-depth fence ring.

## Options considered

### 1. Per-frame fence pool, FIFO ring (chosen)

Pre-allocate `ring_size` × {staging-ref, staging-dis,
command-buffer, fence}. `vmaf_vulkan_import_image()` records
into slot `index % ring_size`, captures the fence, returns.
`vmaf_vulkan_wait_compute()` walks the ring and blocks on
every outstanding fence. `vmaf_vulkan_state_build_pictures()`
waits the slot's fence before exposing the host pointer.
Ring depth = 4 by default — the canonical "double-buffer +
two more for decoder pipelining" depth used by every Vulkan
sample in the Khronos repository.

- **Pros**: Bounded memory, no runtime allocation on the
  hot path, public ABI preserved (the ring is fully internal
  to `VmafVulkanState`), trivially unit-testable, matches the
  pattern feature kernels already use for their own fences.
- **Cons**: Ring depth is fixed at state init — if a caller
  needs more frames in flight than `max_outstanding_frames`,
  the back-pressure stalls show up exactly where the v1 wait
  did. Mitigated by exposing the depth via
  `VmafVulkanConfiguration` (deferred to follow-up #3 in
  the ADR; default 4 is sufficient for FFmpeg's typical
  filter-graph depth of 2–3).
- **Memory**: `2 × ring_size` host-visible staging buffers
  (`stride × h` each). 1080p 8-bit Y plane ≈ 2 MiB per
  buffer ⇒ 16 MiB at default depth. Well below any practical
  budget but worth flagging in the ADR's Consequences.

### 2. Single fence with delayed wait

Move the `vkWaitForFences` from the end of
`vmaf_vulkan_import_image()` to the start of
`vmaf_vulkan_wait_compute()`. Keep one staging-pair, one
command-buffer, one fence.

- **Pros**: Minimal diff vs v1 — single point of change,
  no struct grow.
- **Cons**: Only one frame can be in flight at any time —
  the next `import_image` call still has to wait the prior
  fence before re-recording the command buffer (Vulkan spec
  forbids re-recording a buffer in the pending state). The
  decoder still serialises with libvmaf compute every other
  frame; the gain over v1 is marginal.

Rejected. Doesn't actually solve the parallel-execution
problem — it relocates the wait one function call up the
stack.

### 3. Timeline semaphore with monotonic counter

Replace the fence pool with a single
`VK_SEMAPHORE_TYPE_TIMELINE` semaphore. Each
`import_image` increments a monotonic counter, signals the
semaphore at submission time;
`wait_compute` calls `vkWaitSemaphores` against the latest
counter; `build_pictures` waits the per-frame counter.

- **Pros**: One sync primitive instead of N fences. Maps
  naturally onto FFmpeg's hwframes context — `AVVkFrame::sem`
  is already a timeline semaphore. Eliminates the
  fence-pool teardown ordering trap that v2's ring-drain
  has to manage.
- **Cons**: Requires `VK_KHR_timeline_semaphore` (core in
  1.2). The fork's pinned `api_version` is 1.3 so this is
  available everywhere we run, but the swap touches every
  feature kernel's submit path because timeline semaphores
  cannot be mixed with binary semaphores in a single
  `vkQueueSubmit2` chain — they're either all-binary or
  all-timeline. Bigger blast radius than the ring buys.

Deferred to v3 (ADR-followup) — revisit when a feature
kernel actually needs cross-queue-family transfer where
timeline semaphores are the only correct primitive.

### 4. Stay on v1

- **Pros**: Zero code, zero matrix grow.
- **Cons**: Profile signal (Issue #239) is direct
  evidence the wait dominates the FFmpeg filter wall-clock.

Rejected — accepting the bottleneck indefinitely defeats
the point of v1's "we'll fix it when we have data" framing.

## Decision

Option 1 (per-frame fence ring, FIFO, default depth 4).
Implementation lands as
`core/src/vulkan/import.c` rewrite preserving the public
ABI; the ring is keyed by `frame_index % ring_size`. See
ADR-0251 for the full rationale and the measurement gate
that flips Status to Accepted.

## References

- ADR-0186 (parent — declared this v2 follow-up).
- ADR-0184 (grandparent — pinned the public ABI surface).
- Issue #239 — profile signal.
- Khronos Vulkan-Docs wiki, "Synchronization examples"
  (<https://github.com/KhronosGroup/Vulkan-Docs/wiki/Synchronization-Examples>)
  — canonical fence-ring + timeline-semaphore patterns.
- AMD GPUOpen, "Vulkan Barriers Explained"
  (<https://gpuopen.com/learn/vulkan-barriers-explained/>)
  — fence vs semaphore choice rubric.
- VK-Hpp samples, `samples/12_InitFrameBuffers/` — frames-
  in-flight ring at depth 2 (the lower bound that gives any
  CPU/GPU overlap; we pick 4 for headroom under FFmpeg's
  filter graph).
