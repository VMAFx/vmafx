<!-- markdownlint-disable MD013 -->
# Research-0759: HIP ADM AdmBufferHip by-pointer refactor

**Date**: 2026-05-29
**Status**: Completed — implementation in PR perf/hip-adm-buffer-by-pointer-20260529
**Related**: PR #93 (CUDA F3 fix), PR #95 (HIP audit), PR #96 (CUDA post-audit)

## Background

PR #95 (Research-0755) audited the HIP backend and identified a P1 finding:
`AdmBufferHip` was passed by value (~272 bytes) in four `__global__` kernel
signatures across `adm_csf.hip` and `adm_cm.hip`. This mirrors the CUDA F3 finding.

`AdmBufferHip` (defined in `core/src/feature/hip/integer_adm_hip.h:70–96`) contains:

- 3 `hip_adm_dwt_band_t` (scale-0 bands): 4 × `int16_t *` each = 12 pointers
- 3 `hip_i4_adm_dwt_band_t` (scales 1-3 bands): 4 × `int32_t *` each = 12 pointers
- 4 `int64_t *adm_cm[4]` + 4 `uint64_t *adm_csf_den[4]` = 8 pointers
- 4 void* fields (`data_buf`, `tmp_ref`, `tmp_dis`, `tmp_accum`, `tmp_accum_h`, `tmp_res`, `results_host`) = 7 pointers
- 2 `size_t` fields

Total: ~272 bytes on 64-bit.

## Analysis

`hipModuleLaunchKernel` with the `kernelParams` (void **) calling convention treats
each `kernelParams[i]` as a pointer to the i-th argument value. When `kernelParams[0]`
points to a `AdmBufferHip` struct on the host, the HIP runtime copies the full 272 bytes
into the per-launch argument buffer. On ROCm (GCN/CDNA/RDNA targets), the kernel
argument buffer is in device-accessible memory; each launch must DMA or cache the
arguments. Reducing argument-buffer size directly reduces this overhead.

The struct contains only device pointers that are stable after `init_fex_hip` returns.
None of the pointer values in `AdmBufferHip` change on a per-frame basis: the band
pointers are set up once from `data_buf`, and `adm_cm[]`/`adm_csf_den[]` are
sub-slices of `tmp_res` (also set up once). The only per-frame operation is
`hipMemsetAsync(tmp_res, 0, ...)` which zeroes the accumulator data that the pointers
point to, but does not change the pointers themselves.

Therefore, a single `hipMalloc` + `hipMemcpy(hipMemcpyHostToDevice)` at init time
creates a permanent device-side copy that all kernel launches can reference.

## CUDA twin parity

The CUDA twin in `integer_adm_cuda.c` uses the same pattern: `AdmBufferCuda *buf`
is passed by pointer in kernel signatures, and the device-side copy is maintained
across the extractor's lifetime. This PR brings HIP into parity with that convention.

## Files changed

- `core/src/feature/hip/integer_adm/adm_csf.hip` — 8 signature/access changes
- `core/src/feature/hip/integer_adm/adm_cm.hip` — 10 signature/access changes
- `core/src/feature/hip/integer_adm_hip.c` — `buf_dev` field, malloc, memcpy, free,
  updated helper signatures and args arrays (4 dispatch helpers, 4 call sites)

## Build verification

Build target: `meson setup core/build-hip core -Denable_hipcc=true -Denable_cuda=false -Denable_sycl=false --buildtype=release && ninja -C core/build-hip`

hipcc availability: `docker exec vmaf-dev-mcp which hipcc` (requires ROCm 7.2.3 layer in container per ADR-0543)

## Runtime verification status

PENDING — no AMD GPU available at time of writing. Verification requires:

```text
docker exec vmaf-dev-mcp vmaf \
  --feature integer_adm --backend hip \
  --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8
```

Expected: places=4 parity vs CPU (ADR-0214). The change is numerically transparent
(pointer indirection only; all arithmetic and load patterns are identical).

## Conclusion

The fix is a mechanical refactor with no algorithmic impact. The math argument that
the change is bit-exact: kernel bodies access the same device memory via `buf_ptr->field`
instead of copying `buf.field` onto the device register file. All load addresses are
identical. Runtime verification on AMD hardware is deferred but the change is safe to
merge into a DRAFT PR.
