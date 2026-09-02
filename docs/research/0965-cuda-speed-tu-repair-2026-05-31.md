# Research digest: CUDA SpEED TU repair (ADR-0965, 2026-05-31)

## Summary

This is a targeted repair digest for the three latent bugs found in
`speed_chroma_cuda.c` and `speed_temporal_cuda.c` when ADR-0964 attempted to
wire the CUDA SpEED twins. No novel research was required; the repair is
pattern-matching to existing correct CUDA TUs in the same tree.

## Bug 1 — `CHECK_CUDA` undefined macro

**Root cause.** The SpEED CUDA TUs were authored while an earlier version of
`cuda_helper.cuh` still exposed a `CHECK_CUDA(f, call)` macro. That macro was
removed when Netflix#1420 was addressed: the abort-on-error (`assert(0)`)
behaviour was replaced with errno-return so that recoverable errors (e.g. OOM)
do not hard-kill the host process. The replacement macros are:

- `CHECK_CUDA_GOTO(funcs, CALL, label)` — for use when cleanup state
  (pushed context, allocated buffers) must be unwound before returning.
  Requires `int _cuda_err = 0;` declared once per function.
- `CHECK_CUDA_RETURN(funcs, CALL)` — for use when no cleanup is pending.

The SpEED TUs already had `int _cuda_err = 0;` and `fail:` labels in all
three helper functions (`run_gpu_pipeline`, `run_cpu_linalg`,
`run_score_and_collect` / their `_st` variants). The fix is a mechanical
substitution of `CHECK_CUDA(cu_f, CALL)` →
`CHECK_CUDA_GOTO(cu_f, CALL, fail)`.

**Verification.** After the fix, `grep 'CHECK_CUDA[^_]'` on both TUs returns
empty.

## Bug 2 — `cuMemAllocHost` not in `CudaFunctions`

**Root cause.** The fork's `CudaFunctions` dispatch struct (loaded via
dlopen from `libcuda.so`) exposes `cuMemHostAlloc` (the current CUDA
driver API with a flags argument) and `cuMemFreeHost`, but NOT
`cuMemAllocHost` (an older convenience alias). The SpEED TUs called
`cuMemAllocHost((void **)&ptr, sz)` — which expands via `CHECK_CUDA_GOTO`
to `cu_f->cuMemAllocHost(...)`, referencing a non-existent struct member.

**Fix.** Replace `cuMemAllocHost((void **)&ptr, sz)` with
`cuMemHostAlloc((void **)&ptr, sz, 0x01u)`. The flag `0x01` is
`CU_MEMHOSTALLOC_PORTABLE` — consistent with `picture_cuda.c:150` and
`common.c:334` which use the same pattern.

**Evidence from existing code:**

```c
// core/src/cuda/picture_cuda.c:150
CHECK_CUDA_GOTO(cu_f, cuMemHostAlloc((void **)&data, pic_size, 0x01), fail);
// core/src/cuda/common.c:334
CHECK_CUDA_GOTO(cu_state->f, cuMemHostAlloc(p_buf, size, 0x01), fail);
```

## Scope confirmation — algorithmic correctness

The GPU kernel logic (means/cov/indterm/backward-sub/score kernels), the CPU
eigendecomp + QR factorisation path, and the frame-level orchestration in
`extract_channel()` / `extract_fex_st()` are all algorithmically correct and
unchanged. The bugs were confined to error-handling boilerplate and the
host-allocation API call; no numerical values are affected.

## Parity gate design

The two new parity tests (`test_cuda_speed_chroma_parity.c`,
`test_cuda_speed_temporal_parity.c`) follow the established CUDA parity
template from `test_cuda_motion3_parity.c`:

- 768×432 YUV420P 8bpc synthetic fixture (large enough for SpEED's 4×
  downscale + 5×5 tile blocking, small enough for CI).
- CPU path: `vmaf_use_feature(vmaf, "speed_chroma", NULL)`.
- CUDA path: `vmaf_cuda_state_init` + `vmaf_cuda_import_state` +
  `vmaf_use_feature(vmaf, "speed_chroma_cuda", NULL)`.
- Skip sentinel: `vmaf_cuda_state_init` failure → `out_score = NaN` →
  assertion skipped.
- Tolerance: `1e-4` (ADR-0214 places=4 cross-backend gate).

The chroma test asserts `Speed_chroma_feature_speed_chroma_uv_score` at
frame index 0. The temporal test asserts
`Speed_temporal_feature_speed_temporal_score` at frame index 1 (frame 0
always emits 0.0 by design).
