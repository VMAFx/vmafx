# ADR-0787 — libvmaf Public API Error-Path Consistency Audit

| Field     | Value                                                             |
|-----------|-------------------------------------------------------------------|
| Number    | 0787                                                              |
| Title     | libvmaf Public API Error-Path Consistency Audit                   |
| Status    | Accepted                                                          |
| Date      | 2026-05-29                                                        |
| Authors   | Claude Sonnet 4.6                                                 |
| Tags      | api, error-handling, cuda, sycl, hip, consistency                 |

## Context

A full audit of every `VMAF_EXPORT` function in `core/include/libvmaf/` was
requested to establish ground truth on: (1) the error-return convention,
(2) consistent use of errno codes, (3) silent-error paths (returning 0 on
real failure), (4) void functions that mask errors, and (5) cross-backend
parity. See Research-0787 for the full findings.

## Decision

The audit confirms the following convention is in use and should be maintained
across all backends: `0` on success, `< 0` negative errno on failure. No
VMAF-specific error code namespace is needed.

Six defects were identified (see Research-0787 §6). The following fixes are
approved and should be addressed in a follow-up implementation PR:

1. `vmaf_write_output_with_format()` returns `-EINVAL` for filesystem errors
   — fix to return `-errno`.
2. `vmaf_cuda_state_init()` returns `-EINVAL` for driver-not-found (should
   be `-ENOSYS`) and no-GPU (should be `-ENODEV`).
3. `vmaf_close()` drops return values of `vmaf_framesync_destroy()`,
   `vmaf_thread_pool_wait()`, and `vmaf_picture_unref()` — fix with
   `(void)` casts.
4. `vmaf_cuda_state_free()` has mismatched signature vs. all other backends
   (single-ptr, int-return) — tracked for a future ABI-bump PR; current
   callers are warned.
5. `vmaf_cuda_preallocate_pictures()` lacks the `-EBUSY` guard present in
   SYCL/Vulkan.
6. `vmaf_init()` funnels all sub-init errors to `-ENOMEM` — fix to
   propagate `err`.

## Alternatives considered

- Introducing a VMAF-specific error code enum: rejected. The errno convention
  is already established throughout the codebase and matches the FFmpeg filter
  integration contract. A parallel enum would require a translation layer.

- Treating the `state_free` signature mismatch as a no-op: rejected. The
  single-pointer convention forces callers to manually null the handle. The
  double-pointer convention (used by SYCL, HIP, Metal, Vulkan) is strictly
  safer. Fixing CUDA requires an ABI-break PR with a version bump.

## Consequences

- An implementation PR will address items 1, 2, 3, 5, and 6 without ABI
  impact.
- Item 4 (`vmaf_cuda_state_free` ABI break) is deferred to the next major
  ABI bump.
- The audit output is captured as Research-0787 under `docs/research/`.

## References

- Research-0787:
  `docs/research/research-0787-libvmaf-api-error-path-audit.md`
- Files audited: `core/include/libvmaf/*.h`, `core/src/libvmaf.c`,
  `core/src/cuda/common.c`, `core/src/sycl/common.cpp`,
  `core/src/hip/common.c`, `core/src/picture.c`,
  `core/src/feature/feature_collector.c`
