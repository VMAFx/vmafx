<!-- markdownlint-disable MD013 MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->

# Research-0775: DNN ORT Backend Audit

**Date:** 2026-05-29
**Branch:** research/cuda-13.3-impact-assessment-20260528
**Scope:** `core/src/dnn/` — memory management, thread safety, provider selection, cache lifetime, error coverage

---

## 1. Memory leaks on session close

**Finding: clean.** `vmaf_ort_close()` (`ort_backend.c:720–757`) releases every ORT-owned resource in a careful order:

1. `ReleaseMemoryInfo` — the cached `cpu_mem_info`
2. `AllocatorFree` loop — each `input_names[i]` and `output_names[i]` (ORT-allocated, freed via the ORT allocator)
3. `ReleaseSession` → `ReleaseSessionOptions` → `ReleaseEnv`
4. `free()` for the `input_names`, `output_names`, `input_elem_types`, `output_elem_types` arrays and the session struct itself.

The `calloc` at open time zero-initialises the struct so every pointer is NULL before any ORT call runs; all paths through `ORT_TRY` eventually call `vmaf_ort_close`, which guards each release with a NULL check. One subtle case: when `SessionGetInputName` succeeds for indices 0..k but fails at k+1, the `ORT_TRY` macro fires `vmaf_ort_close`, which iterates `0..n_inputs` — but `n_inputs` was already set to `ni` at line 425 before the partial name-fetch loop (lines 433–436). Names at positions k+1..ni-1 are NULL (calloc), so the `if (sess->input_names[i])` guard at line 732 catches them correctly. **No leak.**

The `in_buf` / `out_buf` scratch buffers in `VmafDnnSession` (`dnn_api.c`) are freed in `vmaf_dnn_session_close`; the `VmafContext`-level `in_buf`/`extra_in_buf`/`feature_name` arrays are freed in `vmaf_dnn_teardown` (`libvmaf.c:663–683`). Both paths handle NULL safely.

---

## 2. Thread safety

**Finding: potential data race on the shared in_buf when n_threads > 1.**

ORT sessions themselves are **not thread-safe for concurrent `Run()` calls** (ORT documentation is explicit; the CPU EP uses an internal thread pool for intra-op parallelism, but the session object must not be called re-entrantly from the host side). The `VmafContext.dnn` struct holds a single `VmafOrtSession *sess` with a single `float *in_buf` scratch buffer.

The per-frame dispatch path in `libvmaf.c:2393–2396` calls `vmaf_ctx_dnn_run_frame()` **before** handing the frame to `vmaf->thread_pool`. This means DNN inference is always serialized relative to the feature-extractor thread pool — the `vmaf_ort_run()` call at lines 1195/1296 is not dispatched into the pool. For the current architecture (one call to `vmaf_ctx_dnn_run_frame` per `vmaf_read_pictures` call, which is caller-serialized), this is safe.

**Latent risk:** if a future caller dispatches `vmaf_read_pictures` from multiple threads concurrently (not the current pattern but not prohibited by the API documentation), the shared `in_buf` and single `VmafOrtSession` would race. There is no mutex protecting `ctx->dnn`. This should be documented as a constraint in the public API header.

The `VmafDnnSession` public API (`dnn_api.c`) is not ctx-bound and carries no locking — callers are expected to serialize `vmaf_dnn_session_run*` against the same session. This is consistent with ORT's own contract but is not stated in `dnn.h`.

---

## 3. Provider selection — does runtime detection match build config?

**Finding: robust, with one build-config vs. runtime gap worth noting.**

EP selection uses ORT's `SessionOptionsAppendExecutionProvider` API: it returns a non-NULL `OrtStatus` if the EP is not linked into the ORT build, and the code falls through to CPU. This means:

- A build with `-Denable_cuda=false` but a CUDA-enabled ORT shared library will still register the CUDA EP (because ORT is the deciding factor, not the VMAF build flag). This is intentional: the code comments state `VMAF_DNN_DEVICE_AUTO` falls through EPs in priority order until one binds.
- Conversely, a build with `-Denable_cuda=true` against a CPU-only ORT library gracefully falls back to CPU (the `OrtStatus` non-null path at line 148–153).

The two-stage `CreateSession` fallback (lines 383–415, ADR-0113) is the right defense: even if `try_append_cuda()` registers the EP without error (hardware check deferred to `CreateSession`), a missing NVIDIA device causes `CreateSession` to fail and the retry path strips the non-CPU EP and creates a CPU-only session. `ep_name` is updated to `"CPU"` on fallback, which is observable via `vmaf_ort_attached_ep()`.

**Minor gap:** `VMAF_DNN_DEVICE_AUTO` does not try OpenVINO CPU in the AUTO chain after OpenVINO GPU fails — it skips directly to ROCm. An explicit `VMAF_DNN_DEVICE_OPENVINO` request does try OpenVINO GPU then OpenVINO CPU, but AUTO omits the CPU-OV fallback. Low severity given CPU EP is always last, but diverges from the comment at line 275 ("OpenVINO (GPU then CPU)").

---

## 4. Cache lifetime

**Finding: correctly scoped.** The `OrtMemoryInfo *cpu_mem_info` is created once in `vmaf_ort_open` (lines 484–490) and stored on the session struct. It is consumed on every `vmaf_ort_infer` and `vmaf_ort_run` call (lines 597, 819) without re-allocation. It is released exactly once in `vmaf_ort_close` (line 728) via `ReleaseMemoryInfo`. The ownership is session-local with no aliasing.

Model-level cache (the `VmafOrtSession` itself) is allocated once at `vmaf_use_tiny_model` / `vmaf_dnn_session_open` and lives for the life of the context or the `VmafDnnSession` struct, respectively. There is no LRU cache or lazy reload path.

---

## 5. Error path coverage

**Finding: thorough, with one class of discarded status worth noting.**

All 14 `ORT_TRY` macro call sites release the `OrtStatus` and call `vmaf_ort_close` before returning `-EIO`. The macro (lines 231–239) is correct: it releases the status before calling close, avoiding a double-free or use-after-free.

`ort_discard_status` is used deliberately in two places: `GetTensorElementType` during the IO type population loops (lines 460, 474). If that call fails, the element type stays at `ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED` (0), which later causes `build_input_tensor` to emit an fp32 tensor regardless — safe degradation, though the silent path masks a malformed model.

The `AllocatorFree` calls in `vmaf_ort_close` wrap the result in `ort_discard_status` — correct, because failing to free an allocator entry is a leak but not a structural error at teardown time.

`vmaf_ort_run` cleanup correctly iterates all `n_inputs`/`n_outputs` entries via `goto cleanup`, releasing all `OrtValue` handles regardless of where in the loop the error occurred.

---

## Summary table

| Area | Status | Severity |
|---|---|---|
| Memory leaks on close | Clean | — |
| Thread safety (shared sess + in_buf) | Latent race if `vmaf_read_pictures` called concurrently | Medium — not current call pattern, no mutex, no doc constraint |
| Provider selection vs. build config | Correct fallback chain | Low — AUTO chain skips OpenVINO CPU (comment mismatch) |
| Cache lifetime | Correctly session-scoped | — |
| Error path coverage | Thorough; two deliberate discards | Low — silent UNDEFINED elem type on malformed model |

## Recommended follow-up items (no fix in this PR)

1. **Document thread-safety contract in `dnn.h`**: add a comment to `vmaf_dnn_session_run` and `vmaf_use_tiny_model` stating that a single session must not be called concurrently from multiple threads.
2. **Fix AUTO chain to include OpenVINO:CPU fallback** after OpenVINO:GPU fails (or update the comment to match actual behavior).
3. **Propagate GetTensorElementType failure** in the IO type population loop rather than silently defaulting to UNDEFINED.
