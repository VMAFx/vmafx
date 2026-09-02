<!-- markdownlint-disable MD013 MD041 -->
- **CUDA and Intel SYCL backend gaps closed** ([ADR-1143](../docs/adr/1143-cuda-intel-backend-gaps.md)):
  - **Python harness backend selection**: `compat/python-vmaf/__init__.py`
    (`ExternalProgramCaller.call_vmafexec` and `call_vmafexec_multi_features`) now reads
    `VMAF_FORCE_BACKEND` (and fallback `VMAF_BACKEND`), mapping it to `--backend <name>`.
  - **CI GPU test scoping**: Scoped `.github/workflows/tests-and-quality-gates.yml` GPU
    test legs away from Netflix CPU golden assertions (`assertAlmostEqual` bit-exact float checks)
    to prevent false failures from ULP-relaxed GPU float differences.
  - **Unified GPU dispatch environment wiring**: `core/src/cuda/dispatch_strategy.c` and
    `core/src/sycl/dispatch_strategy.cpp` now route through `vmaf_gpu_dispatch_env_get`.
    Local once-initialization blocks, duplicate mutexes, and `// NOLINT(concurrency-mt-unsafe)`
    suppressions were removed. `gpu_dispatch_env_cpp23_lib` is linked into
    `libvmaf_feature_static_lib` so all backends, test binaries, and shared libraries inherit it.
  - **Honest CUDA graph dispatch**: `vmaf_cuda_select_strategy` emits a clear warning and
    falls back to `VMAF_CUDA_DISPATCH_DIRECT` when `VMAF_CUDA_DISPATCH=graph` is requested,
    documenting the lack of static graph capture in the CUDA driver API.
  - **Dead CUDA source cleanup**: Removed `core/src/feature/cuda/integer_adm/adm_decouple.cu`
    (superseded by inline decoupling in `adm_csf.cu`) and orphaned
    `core/src/feature/cuda/resolution_dispatch.c` / `.h`.
  - **SYCL Win32 stub error logging**: `core/src/sycl/dmabuf_import.cpp` emits an informative
    error log explaining that DMA-BUF is a Linux kernel primitive before returning `-ENOSYS` on `_WIN32`.
  - **Documentation alignment**: Removed stale claim of TensorRT EP in `docs/index.md` (marked as
    roadmap pointing to `docs/ai/roadmap.md`), documented CUDA graph fallback and zero-copy DMA-BUF
    limitations in `docs/backends/cuda/overview.md`, updated `docs/backends/index.md` and
    `docs/usage/env-vars.md`, and recorded state rows in `docs/state.md`.
