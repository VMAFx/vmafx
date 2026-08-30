# Research-0787 — libvmaf Public API Error-Path Consistency Audit

<!-- markdownlint-disable MD013 -- ADR/research body text; pre-existing long lines per ADR-0864 tail -->

**Date:** 2026-05-29
**Author:** Claude Sonnet 4.6 (automated research agent)
**Scope:** All VMAF_EXPORT functions across `core/include/libvmaf/*.h` and their implementations
**Status:** Complete — findings ready for fix PR

---

## 1. Convention

All `VMAF_EXPORT int` functions uniformly document and implement:

- `0` on success
- `< 0` (negative errno code) on failure

This convention is documented in every public header via `@return 0 on success, or < 0 (a
negative errno code) on error` (e.g., `libvmaf.h` line 106, `libvmaf_cuda.h` line 44,
`libvmaf_sycl.h` line 58). The convention is consistently applied across ~45 public
`int`-returning functions. No VMAF-specific error code namespace exists; the API reuses POSIX
errno values throughout.

---

## 2. Errno Usage — Consistency Findings

### 2a. -EINVAL vs -ENOMEM (generally correct)

NULL-argument guards return `-EINVAL`. Allocation failures return `-ENOMEM`. These are applied
consistently across the CPU, CUDA, SYCL, HIP, Metal, and Vulkan backends.

### 2b. -EAGAIN (correct, intentional)

`vmaf_feature_collector_get_score()` returns `-EAGAIN` when a feature is valid but not yet
written (retroactive scoring; see ADR-0154). This is documented in-source and correct.

### 2c. -EBUSY (fork-added, consistent but undocumented)

`-EBUSY` is returned by `vmaf_sycl_preallocate_pictures()`, `vmaf_vulkan_preallocate_pictures()`,
and `vmaf_use_tiny_model()` / related DNN functions when called a second time after the
resource is already initialised. The CUDA equivalent (`vmaf_cuda_preallocate_pictures()`) does
**not** guard against double-call — it silently overwrites state. This is an inconsistency
across backends.

### 2d. File-open error returns wrong code (BUG)

`vmaf_write_output_with_format()` (`libvmaf.c` lines 2948–2964) returns `-EINVAL` when
`open(2)` or `fdopen(3)` fails. The real cause is a filesystem error (path not found,
permission denied, read-only filesystem), which should surface as `-EIO` or `-errno`. The
errno value is discarded and a hardcoded `-EINVAL` is returned, making the error
indistinguishable from a bad-argument error to callers.

### 2e. CUDA state_init conflates driver-missing with bad-argument (BUG)

`vmaf_cuda_state_init()` returns `-EINVAL` for both:

- driver library load failure (`cuda_load_functions` fails — should be `-ENODEV` or `-ENOSYS`)
- `cuInit()` failure indicating no visible CUDA device (should be `-ENODEV`)

By contrast, `vmaf_hip_state_init()` and `vmaf_sycl_state_init()` correctly return `-ENODEV`
for device-not-found conditions.

---

## 3. Silent-Error Findings

### 3a. vmaf_close() drops return values (DEFECT)

`vmaf_close()` calls `vmaf_framesync_destroy()` and `vmaf_thread_pool_wait()` (both `int`-returning)
without checking or `(void)`-casting their return values. These violations are CERT INT15-C and
NASA Power-of-10 rule (every non-void return value must be checked or explicitly discarded).
The return values are discarded silently, which means thread-pool errors during shutdown are
invisible to the caller.

Additionally, `vmaf_picture_unref()` is called inside `vmaf_close()` without capturing its
return value.

### 3b. vmaf_init() all-paths return -ENOMEM (minor imprecision)

`vmaf_init()`'s error ladder always falls through to `return -ENOMEM` regardless of which
sub-init failed. In practice, the only sub-inits that can fail are allocations, so this is
currently benign. However, if `vmaf_feature_extractor_list_audit()` were to return `-EINVAL`
for a registry bug, that would be silently converted to `-ENOMEM`.

---

## 4. void-Returning Public Functions

The following `VMAF_EXPORT void` functions exist across backends:

| Function | Notes |
| --- | --- |
| `vmaf_model_destroy()` | Correct: destructor, NULL-safe |
| `vmaf_model_collection_destroy()` | Correct: destructor, NULL-safe |
| `vmaf_dnn_session_close()` | Correct: destructor, NULL-safe |
| `vmaf_mcp_close()` | Correct: destructor, NULL-safe |
| `vmaf_sycl_state_free()` | Correct: destructor, NULL-safe, double-ptr |
| `vmaf_sycl_dmabuf_free()` | Correct: free, NULL-safe |
| `vmaf_sycl_profiling_disable()` | Correct: fire-and-forget state reset |
| `vmaf_sycl_profiling_print()` | Acceptable: diagnostic output; errors logged only |
| `vmaf_hip_state_free()` | Correct: destructor, NULL-safe, double-ptr |
| `vmaf_metal_state_free()` | Correct: destructor, NULL-safe, double-ptr |
| `vmaf_vulkan_state_free()` | Correct: destructor, NULL-safe, double-ptr |

None of these silently mask a fatal condition. All destructors are NULL-safe.

---

## 5. Cross-Backend Inconsistencies

### 5a. state_free() signature mismatch (BUG)

`vmaf_cuda_state_free()` has a different signature from all other backend `state_free` functions:

| Backend | Signature |
| --- | --- |
| CUDA | `int vmaf_cuda_state_free(VmafCudaState *cu_state)` — returns `int`, takes single pointer |
| SYCL | `void vmaf_sycl_state_free(VmafSyclState **sycl_state)` — returns `void`, takes double pointer |
| HIP | `void vmaf_hip_state_free(VmafHipState **state)` — returns `void`, takes double pointer |
| Metal | `void vmaf_metal_state_free(VmafMetalState **state)` — returns `void`, takes double pointer |
| Vulkan | `void vmaf_vulkan_state_free(VmafVulkanState **state)` — returns `void`, takes double pointer |

CUDA's `state_free` (a) returns `int` (always 0, never an error), and (b) takes a single
pointer rather than a double-pointer (so callers must manually null the pointer after the call).
The three newer backends (SYCL, HIP, Metal) all use a double-pointer convention that
auto-nulls the handle.

### 5b. preallocate_pictures double-call guard

SYCL and Vulkan return `-EBUSY` on second call. CUDA silently overwrites (see §2c).

### 5c. device-not-found error code

CUDA: returns `-EINVAL` (incorrect). SYCL, HIP: return `-ENODEV` (correct).

---

## 6. Recommendations (no implementation in this PR)

1. **Fix `vmaf_write_output_with_format()` file-open error code**: capture `errno` before
   logging and return `-errno` (or `-EIO` as the canonical I/O fallback). File:
   `core/src/libvmaf.c` lines 2948–2964.

2. **Fix `vmaf_cuda_state_init()` device/driver error codes**: return `-ENOSYS` when
   `cuda_load_functions` fails (driver not present), `-ENODEV` when `cuInit()` fails (driver
   present but no visible GPU). File: `core/src/cuda/common.c` lines 154–183.

3. **Fix `vmaf_close()` unchecked return values**: `(void)`-cast
   `vmaf_framesync_destroy()`, `vmaf_thread_pool_wait()`, and `vmaf_picture_unref()` calls, or
   propagate their errors into the return value. File: `core/src/libvmaf.c` lines 1434–1437.

4. **Normalise `vmaf_cuda_state_free()` to match other backends**: change to
   `void vmaf_cuda_state_free(VmafCudaState **cu_state)` (double-pointer, void return),
   auto-null the handle. This is a **public ABI break** — requires a major ADR and version bump.
   Alternatively: add `vmaf_cuda_state_free2(VmafCudaState **)` and deprecate the old form.

5. **Add `-EBUSY` guard to `vmaf_cuda_preallocate_pictures()`**: match SYCL/Vulkan behaviour
   to prevent silent state overwrite on double-call. File: `core/src/libvmaf.c` lines 354–383.

6. **`vmaf_init()` error propagation**: replace the single `return -ENOMEM` fallthrough with
   `return err` so registry-audit failures (`-EINVAL`) are surfaced correctly. Low priority as
   it is currently only reachable from a programming error, not a runtime condition.

---

## 7. Confirmed NOT Issues

- `-EBUSY` from SYCL/Vulkan/DNN on double-call: intentional and correct.
- `void` destructors: all NULL-safe, none mask fatal errors.
- `-EAGAIN` from `vmaf_feature_collector_get_score`: intentional, documented.
- `return 0.0` in static helper `resolve_feature_score_from_collector()` (lines 1351/1358):
  this is a `double`-returning internal helper, not a public `int` function. The 0.0 is a
  legitimate fallback score, not a false-success errno.
- `vmaf_init()` swallowing sub-init error codes via `-ENOMEM` fallthrough: benign in practice
  (all sub-inits only fail on OOM today), but warrants the fix in rec. 6 for defensive hygiene.

---

## Sources

- `core/include/libvmaf/libvmaf.h`
- `core/include/libvmaf/libvmaf_cuda.h`
- `core/include/libvmaf/libvmaf_sycl.h`
- `core/include/libvmaf/libvmaf_hip.h`
- `core/include/libvmaf/libvmaf_metal.h`
- `core/include/libvmaf/libvmaf_vulkan.h`
- `core/include/libvmaf/dnn.h`
- `core/src/libvmaf.c`
- `core/src/cuda/common.c`
- `core/src/cuda/cuda_helper.cuh`
- `core/src/hip/common.c`
- `core/src/sycl/common.cpp`
- `core/src/picture.c`
- `core/src/feature/feature_collector.c`
