<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1143: CUDA and Intel SYCL Backend Gap Closure

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: Kilian, Antigravity Agent
- **Tags**: cuda, sycl, ci, dispatch, python, docs

## Context

An architectural audit of the CUDA and Intel SYCL backends in VMAFx identified 8
operational gaps across backend dispatch, runtime configuration, build artifacts, and
documentation:

1. **Python Harness Backend Selection Ignored (`GAP-CI-VMAF-FORCE-BACKEND-IGNORED`)**:
   `.github/workflows/tests-and-quality-gates.yml` invoked pytest with
   `VMAF_FORCE_BACKEND=cuda` and `VMAF_FORCE_BACKEND=sycl`, but neither
   `ExternalProgramCaller` nor `compat/python-vmaf/__init__.py` read `VMAF_FORCE_BACKEND`.
   Furthermore, evaluating arbitrary Python tests against GPU backends would fail on
   Netflix CPU-golden assertions (`assertAlmostEqual` bit-exact float checks) rather
   than comparing against calibrated GPU ULP tolerances.
2. **Unwired Dispatch Environment Getter (`GAP-GPU-DISPATCH-ENV-UNWIRED-IN-BACKENDS`)**:
   `core/src/gpu_dispatch_env.cpp` defined the thread-safe cached getter
   `vmaf_gpu_dispatch_env_get` (ADR-0858), but `core/src/cuda/dispatch_strategy.c` and
   `core/src/sycl/dispatch_strategy.cpp` bypassed it with raw `getenv()` calls, redundant
   `pthread_once` / `InitOnceExecuteOnce` guards, and `// NOLINT(concurrency-mt-unsafe)`.
3. **Stale TensorRT EP Documentation (`GAP-DOCS-INDEX-TENSORRT-EP-UNIMPLEMENTED`)**:
   `docs/index.md:92` claimed TensorRT Execution Provider was supported in Tiny-AI, but
   no TensorRT integration exists in `core/src/dnn/ort_backend.c`.
4. **Dead CUDA Source Files (`GAP-BUILD-DEAD-CUDA-ADM-DECOUPLE` and `GAP-BUILD-UNCOMPILED-CUDA-RESOLUTION-DISPATCH`)**:
   `core/src/feature/cuda/integer_adm/adm_decouple.cu` was superseded by inline decoupling in
   `adm_csf.cu` via `adm_decouple_inline.cuh`, and `core/src/feature/cuda/resolution_dispatch.c` / `.h`
   were completely uncompiled and orphaned.
5. **Dishonest CUDA Graph Dispatch Stub (`GAP-CUDA-DISPATCH-STRATEGY-GRAPH-STUB`)**:
   `core/src/cuda/dispatch_strategy.c` parsed `graph` and returned `VMAF_CUDA_DISPATCH_GRAPH_CAPTURE`,
   misleading users even though graph capture is not implemented for driver API CUDA extractors.
6. **Undocumented CUDA Zero-Copy Import Gap (`GAP-CUDA-NO-ZERO-COPY-IMPORT`)**:
   `core/src/cuda/picture_cuda.c` stages picture transfers through pinned host memory rather
   than importing external memory or Linux DMA-BUFs directly.
7. **SYCL DMA-BUF Windows Stub (`GAP-SYCL-DMABUF-IMPORT-WIN32-ENOSYS`)**:
   `core/src/sycl/dmabuf_import.cpp` returned `-ENOSYS` on `_WIN32` silently without explaining
   that DMA-BUF is a Linux kernel primitive.

## Decision

We resolve all 8 gaps in a unified change:

1. **Python Harness & CI Integration**:
   - `ExternalProgramCaller.call_vmafexec_multi_features` and `call_vmafexec` in
     `compat/python-vmaf/__init__.py` read `VMAF_FORCE_BACKEND` (and fallback `VMAF_BACKEND`),
     appending `--backend <val>` to the child `vmaf` command line.
   - Scoped `.github/workflows/tests-and-quality-gates.yml` GPU pytest legs away from the 5
     Netflix CPU golden assertion files (`quality_runner_test.py`, `feature_extractor_test.py`,
     `vmafexec_test.py`, `vmafexec_feature_extractor_test.py`, `result_test.py`).
   - Covered with 4 new unit tests in `python/test/python_harness_coverage_test.py`.
2. **GPU Dispatch Environment Integration**:
   - `core/src/cuda/dispatch_strategy.c` and `core/src/sycl/dispatch_strategy.cpp` invoke
     `vmaf_gpu_dispatch_env_get` directly.
   - Removed local once-initialization blocks, duplicate mutexes, and `// NOLINT` suppressions.
   - Linked `gpu_dispatch_env_cpp23_lib` into `libvmaf_feature_static_lib` in `core/src/meson.build`
     so all consumers, intermediate libraries, and tests link cleanly across all backend configurations.
   - Tested in `core/test/test_gpu_dispatch_runtime.c` with new SYCL dispatch override test cases.
3. **Honest CUDA Graph Dispatch**:
   - In `vmaf_cuda_select_strategy`, if `graph` strategy is parsed, log a clear warning:
     `libvmaf: CUDA graph dispatch requested for '%s' but graph capture is not implemented; falling back to direct`
     and return `VMAF_CUDA_DISPATCH_DIRECT`.
4. **Dead File Cleanup**:
   - Removed `core/src/feature/cuda/integer_adm/adm_decouple.cu`, `core/src/feature/cuda/resolution_dispatch.c`,
     and `core/src/feature/cuda/resolution_dispatch.h`.
5. **SYCL Win32 Informative Error Logging**:
   - In `core/src/sycl/dmabuf_import.cpp`, log an informative error explaining that DMA-BUF is a
     Linux kernel primitive before returning `-ENOSYS` on `_WIN32`.
6. **Documentation & State Tracking**:
   - Corrected `docs/index.md` (TensorRT EP marked as roadmap pointing to `docs/ai/roadmap.md`).
   - Updated `docs/backends/cuda/overview.md`, `docs/backends/index.md`, and `docs/usage/env-vars.md`.
   - Recorded Deferred and Confirmed not-affected rows in `docs/state.md` and updated `docs/rebase-notes.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Implement full CUDA graph capture now** | Enables graph execution for CUDA extractors | High complexity (>800 LOC), requires persistent buffer reuse and graph node rebuild across dynamic pitches and frame sizes | Out of scope for gap closure; honest warning + fallback is the correct maintainable posture |
| **Keep raw `getenv` with NOLINT in CUDA & SYCL** | Zero churn in dispatch code | Duplicates once-cached environment logic; leaves mt-unsafe warnings and dead `gpu_dispatch_env.cpp` | Defeats the purpose of the unified `gpu_dispatch_env` subsystem introduced in ADR-0858 |
| **Run full python test suite on GPU in CI** | Exercises maximum tests with GPU backend | Fails immediately against Netflix CPU golden assertions that check bit-exact float equality | Violates AGENTS.md §8: Netflix golden assertions are CPU-only ground truth; GPU parity belongs in tolerance gates |

## Consequences

- **Positive**:
  - `VMAF_FORCE_BACKEND` is now functional across all Python harness callers.
  - CUDA and SYCL backends share a unified, thread-safe, once-cached dispatch environment parser.
  - Test and documentation surfaces are truthful and eliminate misleading claims about TensorRT and CUDA graph capture.
  - 3 dead files removed from the codebase.
- **Negative**:
  - `VMAF_CUDA_DISPATCH=graph` explicitly emits a warning and falls back to direct execution rather than attempting graph capture.
- **Neutral / follow-ups**:
  - Full CUDA graph capture and CUDA Linux DMA-BUF external memory import remain tracked in `docs/state.md` (Deferred).

## References

- User requirement: Close CUDA + Intel (SYCL) bucket of backend-gap inventory.
- ADR-0181: GPU dispatch strategy abstraction.
- ADR-0214: Cross-backend GPU parity CI gate.
- ADR-0483: GPU dispatch parse deduplication.
- ADR-0858: C++23 isolated static library for `gpu_dispatch_env`.
