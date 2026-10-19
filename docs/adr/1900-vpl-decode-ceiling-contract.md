<!-- markdownlint-disable MD013 MD060 -->
# ADR-1900: Deterministic verification and state contract for VPL decode retry ceiling

- **Status**: Accepted
- **Date**: 2026-10-05
- **Deciders**: VMAFx maintainers
- **Tags**: `tools`, `vpl`, `sycl`, `gpu`, `decode`, `ceilings`, `correctness`, `hiss-21`

## Context

Under [ADR-1287](1287-cli-tool-unbounded-loop-ceilings.md), unbounded `for (;;)` loops across CLI tools
were bounded by explicit scalar ceilings conforming to NASA/JPL Power of 10 Rule 2. For the developer-only
Intel VPL tool `vmaf_vpl` (`core/tools/vmaf_vpl.c`), `vpl_decode_frame()` was capped at
`VPL_DECODE_MAX_ATTEMPTS = 60000` attempts with 1 ms back-off (`usleep(1000)`) on `MFX_WRN_DEVICE_BUSY`.
The ceiling was mathematically derived from the 60-second synchronization timeout (`VPL_SYNC_TIMEOUT_MS = 60000`)
divided by the retry interval (`VPL_DECODE_RETRY_US = 1000`).

However, state row `T-VPL-DECODE-CEILING-UNVERIFIED-2026-09-21` tracked three unverified operational risks:

1. Upstream oneVPL 2.17 (`vpl/mfxvideo.h`) documents `MFX_WRN_DEVICE_BUSY` recovery as "in a few milliseconds"
   without an upper bound; the 60,000-attempt bound had never been tested against physical Intel GPU hardware.
2. The retry counter charged not only for sleeping `MFX_WRN_DEVICE_BUSY` attempts, but also for non-sleeping
   `MFX_ERR_MORE_DATA` bitstream refill attempts.
3. The original loop implementation in `vmaf_vpl.c` was tightly coupled to live hardware sessions (`mfxSession`,
   VA-API surfaces), making it impossible to verify edge cases, retry sequences, or loop exhaustion in
   device-free automated test suites.

Furthermore, a detailed audit of the original `vpl_decode_frame()` revealed two latent correctness defects:

- **Warning frame drop bug**: The loop checked `if (sts == MFX_ERR_NONE && sync)`. If `MFXVideoDECODE_DecodeFrameAsync`
   returned a successful decode accompanied by an informational warning (`sts > 0 && sync != NULL`, such as
   `MFX_WRN_VIDEO_PARAM_CHANGED`), the function dropped the decoded frame, leaked the allocated surface slot,
   and continued spinning in the retry loop.
- **Unclassified transient starvation**: `MFX_WRN_ALLOC_TIMEOUT_EXPIRED` (temporary surface exhaustion) was
   unhandled and fell through to the unclassified retry branch without explicit back-off classification.

## Decision

1. **Extract Decoupled Pure Status Classifier and Frame Loop (`core/tools/vmaf_vpl_core.h`, `.c`)**:
   - Decompose decode execution into two testable pure units:
     - `vpl_classify_decode_status(sts, sync, passing_null)`: Maps raw VPL status codes and synchronization primitives
       into explicit actions:
       - `VPL_DECODE_ACTION_FRAME_READY`: when `(sts == MFX_ERR_NONE || sts > 0) && sync != NULL`. Resolves
         the warning frame drop defect.
       - `VPL_DECODE_ACTION_MORE_DATA`: when `sts == MFX_ERR_MORE_DATA`.
       - `VPL_DECODE_ACTION_RETRY_BUSY`: when `sts == MFX_WRN_DEVICE_BUSY` or `sts == MFX_WRN_ALLOC_TIMEOUT_EXPIRED`.
       - `VPL_DECODE_ACTION_EOF`: when bitstream is empty and drain completes (`sts == MFX_ERR_MORE_DATA`).
       - `VPL_DECODE_ACTION_ERROR`: any unhandled negative status code.
     - `vpl_decode_frame_loop(...)`: Drives decode iterations using a `VplDecodeDriver` interface:

       ```c
       typedef struct VplDecodeDriver {
           size_t (*get_bitstream_length)(void *ctx);
           size_t (*get_buffer_size)(void *ctx);
           int (*is_eof)(void *ctx);
           int (*refill_bitstream)(void *ctx);
           mfxStatus (*decode_async)(void *ctx, int passing_null, mfxFrameSurface1 **out_surf,
                                     mfxSyncPoint *out_sync);
           int (*publish_surface)(void *ctx, mfxSyncPoint sync, mfxFrameSurface1 *out_surf);
           void (*backoff)(void *ctx, unsigned usec);
           void (*log_error)(void *ctx, const char *msg, int code);
           void (*log_exhaustion)(void *ctx, unsigned attempts);
       } VplDecodeDriver;
       ```

   - Retain the exact derived ceiling `VPL_DECODE_MAX_ATTEMPTS = 60000U` and back-off `VPL_DECODE_RETRY_US = 1000U`.
   - On attempt exhaustion, emit the documented diagnostic to stderr:
     `"DecodeFrameAsync yielded no frame after %u attempts\n"` and return `-1`.

2. **Deterministic Device-Free Contract Test Suite (`core/tools/test/test_vmaf_vpl_decode_ceiling.c`)**:
   - Register under Meson `suite : ['fast']` so it executes in every CI lane without GPU hardware.
   - Implement eight deterministic scenarios with simulated time accumulation (no actual sleep):
     - `test_finite_device_busy_retry_succeeds`: 10 busy retries followed by frame delivery.
     - `test_too_low_ceiling_fails_and_sufficient_succeeds`: Proves a too-low ceiling (e.g. 10 attempts) rejects a sequence requiring
       25 attempts, whereas the 60,000 ceiling succeeds.
     - `test_true_no_progress_loop_terminates_at_60000`: Proves an infinite no-progress busy loop terminates after exactly
       60,000 attempts with `-1` and exact stderr text.
     - `test_decoded_frame_ordering_preserved`: Proves 5 consecutive frames with varying retry distributions
       (3, 0, 7, 1, 4 busy retries) maintain strict monotonically increasing frame ordering.
     - `test_warning_with_sync_publishes_frame`: Proves `MFX_WRN_VIDEO_PARAM_CHANGED` with non-null sync delivers
       the frame immediately without spinning or dropping.
     - `test_transient_retryable_statuses`: Proves transient `MFX_WRN_ALLOC_TIMEOUT_EXPIRED` retries and succeeds.
     - `test_hard_error_fails_immediately`: Proves fatal status (e.g. `MFX_ERR_DEVICE_LOST`) exits at attempt 1.
     - `test_exhaustion_exact_diagnostic_message`: Asserts exact stderr byte string emitted on ceiling exhaustion.

3. **Physical Hardware Validation and Smoke Test (`core/tools/test/test_vmaf_vpl_hardware_smoke.sh`)**:
   - Exercise `vmaf_vpl` on physical Intel Arc A380 GPU (`/dev/dri/renderD129`, iHD driver) under both normal
     48-frame baseline and long-GOP elementary streams when the GPU is idle.
   - Register `test_vmaf_vpl_hardware_smoke.sh` under `suite : ['slow', 'gpu']` with `is_parallel : false`.
   - The test auto-detects Intel iHD DRM render nodes, generates transient H.264 test streams, validates both
     zero-copy DMA-BUF and `--fallback` host-upload paths, and skips gracefully (exit 77) when no Intel GPU is present.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Retain unbounded `for (;;)` | Zero risk of premature timeout on extreme contention. | Violates JPL Rule 2 / HISS-02; process hangs indefinitely on hung/dead GPU. | Rejected: fail-closed safety requires a finite termination bound. |
| Time-based `clock_gettime` ceiling only | Measures exact elapsed wall-clock seconds. | Non-deterministic in CI; requires syscall overhead on hot loop; difficult to test hermetically. | Rejected: attempt-count ceiling combined with fixed sleep is deterministic and mockable. |
| Dynamic adaptive back-off (exponential) | Reduces CPU polling overhead during long contention. | Alters timing dynamics tested against VPL specification; oneVPL recommends fixed millisecond retries. | Rejected: 1 ms fixed back-off matches upstream oneVPL guidelines and sync timeout parity. |
| Test only on real GPU hardware | Direct hardware validation. | Fails in containerized and CPU-only CI environments where no Intel GPU is present. | Rejected: device-free contract tests guarantee universal CI regression coverage. |

## Consequences

- **Positive**:
  - `T-VPL-DECODE-CEILING-UNVERIFIED-2026-09-21` is formally resolved with empirical hardware evidence and hermetic contract tests.
  - Correctness defect fixed: frames returned with warning statuses (such as parameter changes) are properly delivered instead of dropped.
  - Fast test suite verifies loop termination, error messages, and frame ordering in < 10 ms without GPU dependencies.
  - Physical GPU smoke test protects Intel Arc / VA-API zero-copy regression in GPU CI lanes.
- **Negative**:
  - Adds one additional header (`vmaf_vpl_core.h`) and source (`vmaf_vpl_core.c`) to `core/tools/`.
- **Neutral / follow-ups**:
  - Public `libvmaf` ABI, FFmpeg filter surface, and Netflix golden assertions remain completely unaffected.

## References

- [ADR-1287](1287-cli-tool-unbounded-loop-ceilings.md) — CLI tool unbounded loop ceilings.
- [Research-1900](../research/1900-vpl-decode-ceiling-verification-2026-10-05.md) — Research and empirical verification of VPL decode retry ceiling.
- [docs/usage/vmaf-vpl.md](../usage/vmaf-vpl.md) — Developer usage guide for `vmaf_vpl`.
- Intel oneVPL Specification 2.17 (`MFXVideoDECODE_DecodeFrameAsync`).
