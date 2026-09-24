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

## Addendum 2026-09-18: reverted, re-applied, measured

- **Reverted.** The implementation (#101, `31a51afb2`) was undone by the next
  merge, #102 (`92ea978a4`), a CUDA ciede change whose branch predated it. The
  kernels passed the struct by value from then until
  `perf/hip-adm-buffer-by-pointer` re-applied the change on the current code
  (T-HIP-ADM-ADR0759-REVERTED-2026-09-18 in `docs/state.md`).
- **Size.** `AdmBufferHip` is 328 bytes, not ~272: the gfx1036 kernel argument
  layout puts the next argument at offset 328. `AdmFixedParametersHip` is 248
  bytes.
- **CUDA parity statement above is wrong.** The CUDA twin passes
  `AdmBufferCuda` by value; the ADR-0756 audit lists those kernels, and none
  was changed.
- **Runtime verification done** on a gfx1036 iGPU (ROCm 7.2): HIP output is
  byte-identical at `%.17g` before and after on 8, 10, 12 and 16-bit inputs,
  odd frame sizes and 1080p clips.
- **Measured effect.** Each of the four kernels' argument segment shrinks by
  320 bytes (856 to 536 on the CSF kernels, 904 to 584 and 968 to 648 on the
  CM kernels). Per-thread scratch and VGPR counts do not change because of the
  pointer. The 936-byte scratch on `adm_cm_line_kernel_8` is VGPR spilling
  (239 spills at the 128-register cap), not a copy of the struct; a
  `__launch_bounds__(128)` experiment raised the cap to 256 and still spilled
  112 registers. End-to-end HIP ADM throughput on a 60-frame 1080p 10-bit clip
  is unchanged within noise (median 29.5 fps before, 30.0 after, 10
  alternating runs, spread 27.4 to 31.1).

The 2026-09-21 section below re-measures two claims made here: the per-thread scratch figure and whether the by-value copy was being spilled. Where the two disagree, the later measurement stands.

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

## 2026-09-25 — collector reconciliation and executable contract

The current collector head already contains the production form through
`7e20ab78d` (#1507), including BUG-092's newer straight-line partial-init
cleanup. Reapplying the older standalone patch would therefore duplicate the
device allocation and restore obsolete `adm_hip_unwind_*` helper names. This
follow-up deliberately makes no production-source change.

The missing protection is now executable in
`core/test/test_hip_adm_buffer_pointer_contract.py`. The fast, device-free gate
checks the four pointer kernel signatures, the four `&s->buf_dev` launch
arrays, the buffer-free reduce kernel, allocation/copy/publication ordering,
and the current dictionary-failure and close release order. Five mutation tests
red-cap by-value parameters, stale host launch wiring, the wrong copy
direction, missing BUG-092 cleanup, and an unnecessary reduce-kernel buffer.
As a historical control, the validator reports sixteen contract failures on
the exact stale collector `92ea978a4`; it reports none on the current source.

ROCm 7.2.53211 rebuilt the HIP targets for the host's idle `gfx1036`. The five
ADM parity/border/tiny-frame executables, the large parity variant, the source
contract and the device-free lifecycle test passed 7/7 serially; the border and
wide-rounding probes reported zero CPU/HIP delta for every printed feature, so
the GPU cases executed rather than taking their no-device skip. The BUG-092
harness initially failed to link because the collector's current score writer
also references `vmaf_feature_collector_append`; adding that no-op symbol to
the already isolated stub set restored the test and its injected dictionary
failure released every allocation while returning `-ENOMEM`.

Fresh `gfx1036` code-object metadata confirms the decided layout still ships:

| kernel | kernarg bytes |
| :--- | ---: |
| `adm_csf_kernel_1_4` | 536 |
| `i4_adm_csf_kernel_1_4` | 536 |
| `i4_adm_cm_line_kernel` | 584 |
| `adm_cm_line_kernel_8` | 648 |
| `adm_cm_reduce_line_kernel_4` | 296 |

No benchmark or retraining run was started; this verification is correctness
and lifecycle work only.
