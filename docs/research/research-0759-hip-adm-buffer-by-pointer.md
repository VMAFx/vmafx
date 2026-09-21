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

## 2026-09-21 — re-application, and what the original write-up got wrong

The convention above landed as `31a51afb2` (PR #101) and was gone again the same
day: `92ea978a4` (PR #102, an unrelated CUDA ciede change cut from an older base)
restored the by-value signatures in both kernel files. The AGENTS.md invariant
note survived the revert, so from then until this entry the file asserted a
pointer-passing contract the kernels did not hold — the shape a rebase trusts.
Branch `fix/bug-hip-adr0759` re-applies the decision on the current tree rather
than reverting the revert, because the surrounding code has moved since May.

Two numbers in the sections above and in ADR-0759 are wrong and are corrected
here (the ADR body is frozen under the Accepted-ADR rule, so the correction
lives in this digest and in `core/src/feature/hip/AGENTS.md`):

- `sizeof(AdmBufferHip)` is **328** bytes on LP64, not ~272. Printed by a
  harness compiled against the real header: two `size_t`, six 32-byte band
  sub-structs, eight further device pointers.
- `sizeof(AdmFixedParametersHip)` is **248** bytes, not ~244. It is still
  passed by value and remains the deferred follow-up.

### Measured effect

`hipcc --genco` (ROCm 7.2.53211), kernel metadata read with
`clang-offload-bundler --unbundle` + `llvm-readelf --notes`. Identical deltas on
`gfx1036`, `gfx1100` and `gfx90a`:

| kernel | kernarg by value | by pointer | delta |
| :--- | ---: | ---: | ---: |
| `adm_csf_kernel_1_4` | 856 | 536 | −320 |
| `i4_adm_csf_kernel_1_4` | 856 | 536 | −320 |
| `i4_adm_cm_line_kernel` | 904 | 584 | −320 |
| `adm_cm_line_kernel_8` | 968 | 648 | −320 |
| `adm_cm_reduce_line_kernel_4` | 296 | 296 | 0 |

320 = `sizeof(AdmBufferHip) - sizeof(void *)`, off every launch of the four
kernels that read the struct. The reduce kernel does not read it and is
unchanged, which is the control.

The by-value copy was also being spilled: `adm_cm_line_kernel_8`'s
`private_segment_fixed_size` drops 920 → 608 bytes on `gfx1036`, 932 → 616 on
`gfx90a` and 664 → 352 on `gfx1100`, and its SGPR count 86 → 78 on `gfx1036`.
VGPR count is unchanged at 128 (it is at the cap). The scratch figure quoted in
earlier notes as "936 bytes on the scale-0 kernel" is that `gfx90a` 932.

### Runtime verification — no longer pending

An AMD device is available on the dev host (`gfx1036`, the Raphael/Granite
Ridge iGPU, ROCm 7.2). A differential harness loaded both HSACO variants into
one process, gave them byte-identical randomised band contents and parameters,
and compared every output buffer:

- `adm_csf_kernel_1_4` — `csf_f` bands 1–3, `int16`
- `i4_adm_csf_kernel_1_4` — `i4_csf_f` bands 1–3, `int32`
- `i4_adm_cm_line_kernel` — the `tmp_accum` per-thread scratch
- `adm_cm_line_kernel_8` — the three `adm_cm[0]` `int64` accumulators

All four are **bit-identical** by value and by pointer, across three
shape/seed configurations (98×50 stride 128, 33×17 stride 36, 160×90 stride
160), with non-zero output everywhere (so the kernels did real work rather than
agreeing on zeros). The earlier "numerically transparent, verification deferred"
claim is now measured rather than argued.

Not verified on this host: an end-to-end `vmaf --backend hip` score run. The
workstation was heavily loaded by sibling agents and a full `enable_hipcc`
build was not run; the kernel-level differential above is the evidence.
