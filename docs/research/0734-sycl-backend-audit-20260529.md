<!-- markdownlint-disable MD013 MD060 -->
# Research-0734 — SYCL Backend Audit (2026-05-29)

**Status:** Complete
**Scope:** `core/src/sycl/` (common.cpp, picture_sycl.cpp, dmabuf_import.cpp, dispatch_strategy) + `core/src/feature/sycl/` (all 19 kernel TUs)
**Audit classes:** (1) Name mangling / lookup safety, (2) Pinned-host / USM leak pattern, (3) Struct-by-value captures in kernel lambdas, (4) Scaffold vs real implementation inventory
**Reviewer summary:** NEEDS-CHANGES on four specific findings; no showstoppers.

---

## 1. Name Mangling / Lookup Safety

**Result: N/A — clean by design.**

SYCL does not use dynamic kernel lookup by string (no `cuModuleGetFunction` equivalent). All kernel submissions are `sycl::queue::submit` lambdas with compile-time captured arguments. The SPIR-V / JIT path resolves kernels via SYCL's internal program cache, not by name. No issue here.

---

## 2. Pinned-Host / USM Leak Pattern

**Result: One confirmed leak site; one potential race.**

### Finding SY-2a — `vmaf_sycl_malloc_host` not freed on `init_fex_sycl` partial failure

**Severity:** Medium
**Files:**

- `/home/kilian/dev/vmaf/core/src/feature/sycl/float_adm_sycl.cpp` lines 676–719
- `/home/kilian/dev/vmaf/core/src/feature/sycl/float_vif_sycl.cpp` (analogous pattern)

In `float_adm_sycl.cpp::init_fex_sycl`, USM host allocations (`h_ref_raw`, `h_dis_raw`, `h_accum[scale]`) and device allocations (`d_ref_raw`, `d_dis_raw`, etc.) are all allocated before the null-check block at line 712. If any intermediate allocation fails with NULL, `init_fex_sycl` returns `-ENOMEM` but does **not** call `close_fex_sycl`. The null-checks at lines 712–719 therefore silently leak all successfully-allocated USM pointers that precede the first NULL.

The CUDA equivalent (`vmaf_cuda_kernel_readback_free` / `init` unwind pattern) uses explicit goto-style unwind on each alloc failure. The SYCL `init` functions instead batch all allocations and check at the end — the `close_fex_sycl` path does guard with `if (ptr)` checks so it is safe to call on a partially-initialised state, but it is never called from the failure return path.

**Recommendation:** On the `return -ENOMEM` path, call `close_fex_sycl(fex)` before returning, or refactor to check each allocation immediately and free any already-allocated USM before returning. Pattern:

```c
if (!s->h_ref_raw) { close_fex_sycl(fex); return -ENOMEM; }
```

### Finding SY-2b — `vmaf_sycl_flush_pending_imports` only frees via `zeMemFree`; misses `vmaf_sycl_free` path

**Severity:** Low
**File:** `/home/kilian/dev/vmaf/core/src/sycl/common.cpp` lines 1028–1049

`vmaf_sycl_flush_pending_imports` directly calls `zeMemFree` on Level Zero–imported DMA-BUF pointers. This is correct for the L0-imported path (`vmaf_sycl_dmabuf_import` uses `zeMemAllocDevice` directly). However, the asymmetry with the rest of the codebase — where `vmaf_sycl_free` routes through `sycl::free` — means a future caller that accidentally defers a SYCL-USM pointer (not an L0-imported pointer) via `vmaf_sycl_defer_import_free` would silently miscall `zeMemFree` on a SYCL-USM pointer, causing undefined behaviour. The code comment in `dmabuf_import.cpp` is correct about usage intent, but there is no runtime assertion.

**Recommendation:** Add an assertion or type-tag to `pending_import_ptrs` that verifies L0-only provenance. At minimum a comment at `vmaf_sycl_defer_import_free` call sites documenting the L0-only contract.

---

## 3. Struct-by-Value Captures in Kernel Lambdas

**Result: Two confirmed problematic TUs; others clean.**

All kernels use explicit local scalar aliases before capture (the `e_xxx = xxx` aliasing pattern), correctly pulling values out of parent structs before the `[=]` capture. This is the right pattern and is applied consistently in: `float_adm_sycl.cpp`, `integer_adm_sycl.cpp`, `float_vif_sycl.cpp`, `integer_vif_sycl.cpp`, `integer_cambi_sycl.cpp`, `integer_psnr_sycl.cpp`, `integer_motion_sycl.cpp`, `float_psnr_sycl.cpp`, `float_motion_sycl.cpp`, `integer_ms_ssim_sycl.cpp`, `integer_ssim_sycl.cpp`, `integer_psnr_hvs_sycl.cpp`, `ssimulacra2_sycl.cpp`.

### Finding SY-3a — `speed_temporal_sycl.cpp` and `speed_chroma_sycl.cpp` bypass `vmaf_sycl_get_queue_ptr` and cast the opaque struct member directly

**Severity:** Medium
**Files:**

- `/home/kilian/dev/vmaf/core/src/feature/sycl/speed_temporal_sycl.cpp` lines 286, 352, 402, 542
- `/home/kilian/dev/vmaf/core/src/feature/sycl/speed_chroma_sycl.cpp` lines 285, 341, 401, 582

Both files cast `s->sycl_state->queue` directly:

```cpp
sycl::queue &q = *(sycl::queue *)s->sycl_state->queue;
```

This accesses the `VmafSyclState::queue` field by-address through `void*` reinterpret. The `VmafSyclState` struct is defined in `common.cpp` and intended to be opaque to external code; the public API exposes `vmaf_sycl_get_queue_ptr()` for exactly this purpose. The direct cast is technically correct since the worktree is a single binary, but it breaks the opacity contract and creates a silent coupling to the `queue` field's position in the struct.

More importantly, these two TUs bypass `vmaf_sycl_get_combined_queue` and therefore **do not participate in the combined command graph** (`vmaf_sycl_graph_register` is not called). They access `state->queue` (the primary queue) rather than `state->combined_queue`, so they execute on a different queue than the rest of the extractors and cannot benefit from graph replay.

**Recommendation:** Replace the direct struct-member cast with `vmaf_sycl_get_queue_ptr(s->sycl_state)`. Evaluate whether these extractors should register with the combined graph or remain on a standalone queue; if standalone, document the architectural reason.

### Finding SY-3b — `speed_temporal_sycl.cpp` kernel captures `double` accumulator in `launch_means` / `launch_cov`

**Severity:** Medium (precision concern)
**File:** `/home/kilian/dev/vmaf/core/src/feature/sycl/speed_temporal_sycl.cpp` lines 62–130

The `launch_means` and `launch_cov` kernels use `double acc = 0.0` in a per-thread accumulator loop. On devices without native fp64 (Intel Arc A-series iGPUs without the `fp64` aspect), this triggers software emulation — a significant performance regression (10–100× slower than fp32). The comment in `common.cpp` at line 215 explicitly documents that all kernels are designed to be fp64-free, yet these two kernels violate that invariant.

Unlike `integer_adm_sycl.cpp` which proactively avoids `sycl::reduction<double>` and documents this (ADR-0220), `speed_temporal_sycl.cpp` uses `double` without conditional guards.

**Recommendation:** Either gate the `double` path on `vmaf_sycl_has_fp64(state)` and provide an fp32 fallback path, or convert the accumulator to `float` with a comment justifying the precision tradeoff. The SpEED metric itself is coarse (ordinal ranking), so fp32 accumulation is likely acceptable.

---

## 4. Scaffold vs Real Implementation Inventory

| Extractor | Status | Notes |
|---|---|---|
| `float_adm_sycl` | **Real** | Full 4-scale DWT (fp32), decouple, CSF, CM. 16 kernel launches per frame. Does not use shared-frame model (multi-scale layout). |
| `integer_adm_sycl` | **Real** | Full 4-scale CDF 9/7 DWT (int32 + int64 Q31), decouple, CSF, CM. Uses `d_div_lookup` LUT, Q31 gain limiting to avoid fp64. ~1758 lines. |
| `float_vif_sycl` | **Real** | Multi-scale VIF (fp32). |
| `integer_vif_sycl` | **Real** | Multi-scale VIF (int32 fixed-point). Participates in combined graph. |
| `float_motion_sycl` | **Real** | Temporal SAD-based motion (fp32). Collect: `qptr->wait()` — legitimate post-frame join. |
| `integer_motion_sycl` | **Real** | Temporal SAD (int32). Participates in combined graph via `vmaf_sycl_graph_wait`. |
| `integer_motion_v2_sycl` | **Real** | Same as above, 5-frame window. |
| `float_psnr_sycl` | **Real** | Per-pixel SSE reduction (fp32). |
| `integer_psnr_sycl` | **Real** | Per-pixel SSE (int64 atomics). Uses `sycl::range<2>` (appropriate — no sub-group primitives). |
| `integer_ssim_sycl` | **Real** | SSIM Gaussian window, 3-scale. Two `qptr->wait()` calls in collect — D2H barrier. |
| `integer_ms_ssim_sycl` | **Real** | Multi-scale SSIM. `q.wait()` in collect inside scale loop — intermediate shared buffers require sequential scale execution (documented in comment). |
| `integer_psnr_hvs_sycl` | **Real** | 8×8 DCT + CSF masking. Single `q.wait()` before final readback. |
| `integer_cambi_sycl` | **Real** | CAMBI preprocess + derivative + decimate pipeline. Mixed `q.wait()` usage — most are legitimate H2D/D2H serialization points; see SY-4a below. |
| `integer_ciede_sycl` | **Real** | CIEDE2000 color-difference. |
| `integer_moment_sycl` | **Real** | Second-order moments (Y plane). Uses `sycl::range<2>` — appropriate. |
| `ssimulacra2_sycl` | **Real** | SSIMULACRA2 multi-scale. Two `q.wait()` per scale — documented pattern. |
| `speed_temporal_sycl` | **Real** (with caveats) | Full SpEED-Temporal GPU pipeline. Struct opacity issue (SY-3a) and fp64 issue (SY-3b). Seven `q.wait()` calls in frame loop — hot path. |
| `speed_chroma_sycl` | **Real** (with caveats) | Full SpEED-Chroma. Same struct opacity issue (SY-3a). Seven `q.wait()` calls in frame loop. |
| `float_vif_sycl` | **Real** | fp32 VIF variant. |

**All 19 feature extractors have real kernel implementations.** There are no stub-only registrations in the SYCL feature directory. The CAMBI, SpEED, and SSIMULACRA2 ports are complete.

### Finding SY-4a — `speed_temporal` / `speed_chroma` excessive `q.wait()` in frame loop

**Severity:** Medium (performance)
**Files:**

- `/home/kilian/dev/vmaf/core/src/feature/sycl/speed_temporal_sycl.cpp` — 7 `q.wait()` calls per frame (`run_channel_st` + `score_aggregate_st`)
- `/home/kilian/dev/vmaf/core/src/feature/sycl/speed_chroma_sycl.cpp` — 7 `q.wait()` calls per frame

These are CPU-serializing sync points in the hot frame loop. The SYCL 2020 spec §4.6.5 recommends event-chaining over repeated queue drains. The CUDA audit (PR #96 research-0563) found the equivalent CUDA pattern to cost 3–8 ms per frame on Arc.

However, inspection shows most of these `q.wait()` calls are load-bearing:

- The `q.wait()` after `launch_cov` (line 370) is required because `s->h_cov_mat` is read by `speed_internal_compute_eigenvalues` on the CPU.
- The `q.wait()` after `launch_solve` (line 392) is required because `h_indterm` is read immediately after.
- The `q.wait()` after `q.memcpy(d_eigenvalues)` (line 396) is immediately followed by `launch_score` which reads it — since the queue is in-order this `q.wait()` is technically redundant (the in-order queue guarantees the memcpy completes before the next submission).

**Recommendation:** At lines 395–397 of `speed_temporal_sycl.cpp`: the `q.memcpy` + `q.wait()` before `launch_score` can be eliminated. The in-order queue serializes the memcpy → `launch_score` dependency automatically. Save one CPU-side sync per channel per frame (~2 saves per frame across ref/dis channels).

---

## 5. Additional Findings

### Finding SY-5a — Readback fallback path in `dmabuf_import.cpp` uses primary queue with `q->wait()`

**Severity:** Low
**File:** `/home/kilian/dev/vmaf/core/src/sycl/dmabuf_import.cpp` line 278

`vmaf_sycl_import_va_surface_readback` issues `q->wait()` (the primary queue) after the H2D upload loop. This blocks the primary queue — it should use the copy queue for uploads (as the main `vmaf_sycl_shared_frame_upload` does). Using the primary queue here also prevents DMA/compute overlap that the double-buffer design in `common.cpp` was built to enable.

**Recommendation:** Replace `q` with `state->copy_queue` in the readback path and use `vmaf_sycl_upload_plane()` or `vmaf_sycl_shared_frame_upload()` directly instead of manually calling `q->memcpy`.

### Finding SY-5b — `work_group_size` never queried; hard-coded WG sizes not justified in all TUs

**Severity:** Low
**Files:** `integer_psnr_sycl.cpp`, `integer_moment_sycl.cpp`

These two TUs use `sycl::range<2>` with no work-group dimension (flat `parallel_for`), which is fine for independent work. However, `integer_psnr_sycl.cpp` line 141 uses `sycl::atomic_ref` inside a plain `parallel_for<range>` — this is correct and avoids the need for sub-group reductions, but the choice is not commented.

The larger TUs (`integer_adm_sycl.cpp`, `integer_vif_sycl.cpp`) use `WG_X=32, WG_Y=16` consistently without querying `preferred_work_group_size_multiple`. The SYCL 2020 spec §4.9.1.2 recommends querying `kernel::get_info<sycl::info::kernel_device_specific::preferred_work_group_size_multiple>` at runtime. The hard-coded 32 matches Intel GPU sub-group width but is not portable.

**Recommendation:** Add a `static_assert` or runtime check that `WG_X * WG_Y` is a multiple of `preferred_work_group_size_multiple` when in debug mode. This is non-blocking for current Intel-only targets but matters for AdaptiveCpp + non-Intel devices (ADR-0407).

### Finding SY-5c — `cgh.barrier()` vs `ext_oneapi_submit_barrier` consistency

**Severity:** Low
**File:** `/home/kilian/dev/vmaf/core/src/sycl/common.cpp` lines 839, 845, 910, 915

`vmaf_sycl_graph_submit` uses `q.ext_oneapi_submit_barrier({event})` for cross-queue synchronization (upload/de-tile → compute). This is the correct SYCL extension for event-based barriers on in-order queues. The in-kernel `item.barrier(sycl::access::fence_space::local_space)` in several kernels (e.g., `float_adm_sycl.cpp:539`) is also correct — local-space fence for SLM synchronization.

No `cgh.barrier()` (deprecated global barrier) calls found. This category is clean.

---

## 6. Summary Table

| Finding ID | File(s) | Category | Severity | Status |
|---|---|---|---|---|
| SY-2a | `float_adm_sycl.cpp`, `float_vif_sycl.cpp` | USM leak on partial init failure | Medium | Open |
| SY-2b | `common.cpp` | `zeMemFree` / `sycl::free` asymmetry contract | Low | Open |
| SY-3a | `speed_temporal_sycl.cpp`, `speed_chroma_sycl.cpp` | Struct opacity bypass; combined-graph exclusion | Medium | Open |
| SY-3b | `speed_temporal_sycl.cpp` | fp64 in kernel on potentially non-fp64 device | Medium | Open |
| SY-4a | `speed_temporal_sycl.cpp`, `speed_chroma_sycl.cpp` | Redundant `q.wait()` in frame loop | Medium (perf) | Open |
| SY-5a | `dmabuf_import.cpp` | Readback fallback on primary queue; prevents DMA/compute overlap | Low | Open |
| SY-5b | Multiple | `preferred_work_group_size_multiple` not queried; hard-coded WG | Low | Open |
| SY-5c | `common.cpp` | Barrier usage | Clean | N/A |

---

## References

- SYCL 2020 spec §4.6.5 (event dependencies)
- SYCL 2020 spec §4.9.1.2 (`preferred_work_group_size_multiple`)
- ADR-0220 (fp64-free kernel design mandate)
- ADR-0407 (AdaptiveCpp second toolchain)
- research-0086 (SYCL toolchain audit 2026-05-08)
- research-0563 (HIP extractor audit)
