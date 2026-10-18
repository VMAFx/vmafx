<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3352: A SYCL device waits on and signals Vulkan timeline semaphores on its queue

- **Status**: Accepted
- **Date**: 2026-10-18
- **Deciders**: maintainer (standing order to work the 0.1.0 milestone, ledger vmafx-next-20261018-1114)
- **Tags**: api, rc4, gpu, sycl, vulkan, zero-copy

## Context

[ADR-2152](2152-vmafx-vulkan-frame-import.md) let a SYCL device read Vulkan
frames through the dma-buf route, but it refused `VMAFX_FENCE_VULKAN_SEMAPHORE`
fences: the producer handed over a `sync_file` of its write, which the library
checked on the host, and signalled its own timeline after a HOST release
fence. ADR-2152 listed a device-side wait as a follow-up
(`T-VULKAN-IMPORT-SYCL-HOST-FENCES-2026-10-06`). The pinned oneAPI DPC++
runtime has the `sycl_ext_oneapi_bindless_images` external-semaphore
interface: a semaphore imported from a file descriptor
(`external_semaphore_descriptor<resource_fd>`), and
`queue::ext_oneapi_wait_external_semaphore()` /
`ext_oneapi_signal_external_semaphore()` with a timeline value. Level Zero
imports a Vulkan timeline exported as `OPAQUE_FD` on the Arc A380.

## Decision

A SYCL device on Linux takes a `VMAFX_FENCE_VULKAN_SEMAPHORE` (timeline,
`OPAQUE_FD`) in `acquire` and in up to three `acquire_more` entries and waits
on each on the library queue before the frame's first read, and
`vmafx_frame_signal_on_release()` with a Vulkan timeline (up to three per
frame) signals it on the queue behind the frame's last reader, the same
contract as the CUDA lane ([ADR-2152](2152-vmafx-vulkan-frame-import.md)
item 5). The library duplicates the descriptor before the import because the
runtime closes the one it imports, so the caller keeps ownership of its own
as on CUDA. A further acquire of any other kind and OPTIMAL images stay
refused by name; Windows handles stay refused as on every lane.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| DPC++ external semaphores on the library queue (chosen) | No host wait in either direction; the same API as the SYCL queue the twins use; the frame's existing read join orders the signal | Linux only until a Windows Arc device runs the tests | — |
| Level Zero directly (`zeDeviceImportExternalSemaphoreExt()`, `zeCommandListAppendWaitExternalSemaphoreExt()`) | Measured on the A380 in ADR-2152 | A second command path beside the SYCL queue, ordered by hand against the twins' work | The SYCL interface wraps it and keeps one queue |
| Keep the host `sync_file` check and HOST release | Nothing to build | The producer waits for the host and the host polls; the state row stays open | The runtime supports the device path |

## Consequences

- **Positive**: Vulkan producers feed a SYCL device with no host wait; the
  same descriptor works on CUDA and SYCL devices.
- **Negative**: a semaphore import per frame (the CUDA lane's per-frame
  import cost, `T-CUDA-VULKAN-IMPORT-PER-FRAME-INTEROP-2026-10-06`, applies
  here too).
- **Neutral / follow-ups**: `test_vmafx_import_vulkan_sycl_fence` runs the
  acquire and release arms with the timeline; its planted switches
  (`VMAFX_TEST_SKIP_ACQUIRE_WAIT`, `VMAFX_TEST_EARLY_RELEASE`) must score
  frames wrong. HIP still refuses Vulkan semaphores
  (`T-HIP-ROCM-NO-SYNC-FILE-SEMAPHORE-2026-10-06`).

## References

- [ADR-2152](2152-vmafx-vulkan-frame-import.md), [ADR-2091](2091-vmafx-sycl-device-frames.md), [ADR-1929](1929-vmafx-device-frames-fences.md), [Research-2162](../research/2162-vmafx-vulkan-frame-import.md).
- State row `T-VULKAN-IMPORT-SYCL-HOST-FENCES-2026-10-06` (docs/state.md).
- Order vmafx-next-20261018-1114 (paraphrased): take the next open item of the 0.1.0 milestone and land it.
