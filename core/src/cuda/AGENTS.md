<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# AGENTS.md — core/src/cuda

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

Orientation for agents working on CUDA backend runtime. Parent:
[../../AGENTS.md](../../AGENTS.md).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `picture_cuda.c`, `/core/src/libvmaf.c`, `/core/src/meson.build`, `/ffmpeg-patches/src/vf_vmafx.c` | [adrs-build-and-plane-launches](AGENTS.d/adrs-build-and-plane-launches.md) | CUDA governing ADRs, FFmpeg vmafx filter contract, kernelParams count parity, and full-plane D2H download mask. |
| `cuda_helper.cuh`, `kernel_template.h`, `drain_batch.h`, `/core/src/feature/cuda/integer_psnr_hvs_cuda.c`, `/core/src/feature/cuda/integer_ms_ssim_cuda.c`, `/core/src/feature/cuda/integer_psnr_cuda.c` | [extractor-and-template-invariants](AGENTS.d/extractor-and-template-invariants.md) | Graceful CHECK_CUDA macros, persistent PSNR-HVS staging, MS-SSIM partials, chroma PSNR, and kernel_template.h. |
| `/core/src/meson.build`, `common.c`, `picture_cuda.c`, `drain_batch.c`, `kernel_template.h` | [nvcc-flags-and-lifecycle](AGENTS.d/nvcc-flags-and-lifecycle.md) | Shared strict-FP fatbin flags (--fmad=false), context-current resource teardown, drain_batch owner, and unwinds. |
| `common.c`, `common.h`, `picture_cuda.c`, `dispatch_strategy.c`, `/core/src/gpu_picture_pool.cpp`, `/core/src/meson.build`, `/core/include/libvmaf/libvmaf_cuda.h` | [scope-ground-rules-and-runtime](AGENTS.d/scope-ground-rules-and-runtime.md) | CUDA runtime scope, sm_80 floor, non-blocking upload streams, synchronous cuMemFree, and state_free ownership. |
| `import_*.c`, `import_convert.cu`, `vmafx_cuda*.h`, `cuda_device_ptr.cuh`, `common.c`, `picture_cuda.c` | [vmafx-frames-and-device-lint](AGENTS.d/vmafx-frames-and-device-lint.md) | VMAFx CUDA frame import/fences/pools, goto-free teardown helpers, VMAF_CUDA_DPTR macro, and .cu/.cuh lint rules. |
