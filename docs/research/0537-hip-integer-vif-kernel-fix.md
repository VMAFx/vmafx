# Research digest: HIP integer-VIF kernel fault analysis (ADR-0537)

- **Date**: 2026-05-18
- **Author**: Claude (Anthropic), reviewed by lusoris
- **Scope**: Root-cause analysis of the GPU memory access fault that
  ADR-0530 documented but did not fix — the
  `vmaf_fex_integer_vif_hip` extractor crashing on the first frame
  whenever the model-driven dispatch picked it on AMD ROCm.

## Method

1. **Reproduce** the fault in `vmaf-dev-mcp` against AMD gfx1036 with
   `AMD_LOG_LEVEL=3`.  Capture the `rocvirtual.cpp` argument-dump for
   every kernel launch.
2. **Compare against the CUDA twin**
   (`core/src/feature/cuda/integer_vif/{filter1d.cu,vif_statistics.cuh}`)
   to identify structural divergence in (a) kernel signature, (b) host
   `args[]` array, (c) filter-coefficient handling, (d) downsample
   pipeline.
3. **Comparable HIP extractors** (`integer_motion_hip.c`,
   `float_psnr_hip.c`) for the established HIP launch-arg idiom and
   per-frame HtoD staging precedent.
4. **gdb backtrace** of the post-launch SIGSEGV (after applying the
   first three fixes) to confirm the dispatch was now reaching
   `collect_fex_hip` rather than faulting inside the kernel.

## Findings

### 1. Filter table host-pointer (kernel arg)

The pre-fix kernel signature was:

```cpp
__global__ void filter1d_8_vertical_kernel_uint32_t_17_9(
    VifBufferHip buf,
    const uint8_t *__restrict__ ref_in,
    const uint8_t *__restrict__ dis_in,
    int w, int h,
    const uint16_t filter[18])  // <- decays to const uint16_t*
{ ... filter[k + HALF] ... }
```

Host launch:

```c
void *args_vert[] = {buf, &ref_in, &dis_in, &w, &h,
                     (void *)vif_filter1d_table[0]};  // <- BUG
```

`vif_filter1d_table` is a **host-side** `static const uint16_t[4][18]`
defined in `core/src/feature/integer_vif.h`.  `vif_filter1d_table[0]`
decays to a host pointer.  `hipModuleLaunchKernel` then copied that
host address into the kernel argument region, and the GPU dereferenced
it on the first `filter[fi]` load.  AMD ROCm faulted with:

```text
Memory access fault by GPU node-1 ... on address 0x55... Reason: Page
not present or supervisor privilege.
```

CUDA hides this defect because the CUDA twin passes the filter as a
**by-value struct** (`filter_table_stuct` containing a 4×18 array) —
the entire 144-byte payload is copied into the kernel argument region,
so there is no pointer for the GPU to dereference.  HIP module-launch
does support struct-by-value args, but the existing HIP kernel chose
the pointer signature.

**Fix:** allocate a device buffer at init, `hipMemcpy` the table to
it once, pass `&s->vif_filt_dev` (address of the variable storing the
device pointer) in `args[]`.

### 2. Filter half-width parsed from the wrong kernel-suffix number

Kernel names like `filter1d_16_vertical_kernel_uint2_17_9_0` encode
**three** integers: `fwidth_0=17`, `fwidth_1=9`, `scale=0`.  These
come from the CUDA twin's templated instantiations
(`FILTER1D_16_VERT(uint2, 17, 9, 0)`) where the second number is the
**downsample filter width** (used to compute `accum_ref_rd` /
`accum_dis_rd`), not the half-width of the main filter.

The pre-fix HIP kernel ignored that and used:

```cpp
const int HALF = 9;  // for the "17_9_0" kernel
for (int k = -HALF; k <= HALF; ++k) {  // k = -9..9 -> 19 iterations
    uint16_t coeff = filter[k + HALF];  // filter[0..18]
    ...
}
```

This reads 19 filter coefficients from an 18-entry `vif_filter1d_table[0]`
— off-by-one OOB read on every output pixel.  The actual filter widths
are:

```c
static const int vif_filter1d_width[4] = {17, 9, 5, 3};
// → half-widths: 8, 4, 2, 1
```

**Fix:** use `vif_filter1d_width[scale] / 2` for the half-width.

### 3. Missing rd-filter downsample-write path

The CUDA twin's scale-0/1/2 kernels compute the `*_convol` channels
in addition to the `*` channels:

```cpp
if (fi >= (fwidth_0 - fwidth_1) / 2 &&
    fi < (fwidth_0 - (fwidth_0 - fwidth_1) / 2)) {
    accum_ref_rd[off] += fcoeff_rd * imgcoeff_ref;
    accum_dis_rd[off] += fcoeff_rd * imgcoeff_dis;
}
// ...
ref[(y / 2) * rd_stride + (x / 2)] =
    (uint16_t)((accum_ref_rd[off / 2] + 32768) >> 16);
dis[(y / 2) * rd_stride + (x / 2)] =
    (uint16_t)((accum_dis_rd[off / 2] + 32768) >> 16);
```

The pre-fix HIP kernel did neither.  Scales 1–3 then read from
`s->buf.ref` / `s->buf.dis` (the rd half-res planes) which were
uninitialised device memory — garbage in, garbage out, with crash
probability undefined.

**Fix:** apply the rd filter in the horizontal pass (matching the CPU
`subsample_rd_8` / `subsample_rd_16` reference); decimate-by-2 in the
even-thread write path.

### 4. Picture buffer host pointer (separately reachable)

`VmafPicture` arrives as `VMAF_PICTURE_BUFFER_TYPE_HOST` for HIP
(there is no HIP picture pool yet — `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE`
is reserved in ADR-0530 for future work).  The pre-fix HIP submit
passed `ref_pic->data[0]` directly to the kernel.  Identical bug
class to #1; would have faulted as soon as the filter pointer was
fixed.

**Fix:** allocate `s->ref_in_dev` / `s->dis_in_dev` at init, run
`hipMemcpy2DAsync` from `ref_pic->data[0]` into the device staging
buffer before launching the scale-0 kernel.  Established precedent
in `integer_motion_hip.c::msh_submit_inputs()` at lines 372–373.

### 5. `feature_name_dict` ABI corruption (separately discovered)

After fixing the four kernel defects above, gdb showed a SIGSEGV
inside `vmaf_dictionary_get` reading `dict->cnt`:

```text
Thread 1 "vmaf" received signal SIGSEGV
0x00007f... in vmaf_dictionary_get (dict=0x7fff..., key="VMAF_integer_feature_vif_scale0_score") at dict.c:40
40    for (unsigned i = 0; i < d->cnt; i++) {
```

The dict pointer value was `0xffffffffa028f710` — a 32-bit value
sign-extended to 64-bit.  The cause: `integer_vif_hip.c` was missing
`#include "feature_name.h"` (the header declaring
`vmaf_feature_name_dict_from_provided_features()` as returning
`VmafDictionary *`).  Without the declaration, GCC's implicit-int
rule treated the return as `int`, truncating the 64-bit pointer to
32 bits at the call site.

**Fix:** add `#include "feature_name.h"`.  This was missed because
the legacy HIP scaffold copy-pasted only the includes the original
scaffold-posture (-ENOSYS) needed; the real init path requires more.

## Decision

Apply all five fixes in a single PR (ADR-0537).  Re-enable
`VMAF_FEATURE_EXTRACTOR_HIP` on `vmaf_fex_integer_vif_hip`.

Adjacent latent issues surfaced during the rebuild are bundled:

- Missing HSACO entries in `hip_kernel_sources` for the ADR-0533
  registration sweep (seven extractors referenced `_hsaco` symbols
  the build never produced).
- Weak-stub `_hsaco` symbols for the four ADM kernels that fail to
  compile standalone because they depend on CUDA-helper macros
  (`uint64_cu`, `warp_reduce`, `VMAF_CUDA_THREADS_PER_WARP`).
- Include path for `hipcc --genco` extended to find `config.h`
  (in `build_dir`) and `integer_*_hip.h` (in `feature/hip`).

## Per-frame parity numbers (Netflix golden pair, 576×324, 48 frames)

| Scale | HIP    | CPU    | Δ        |
|-------|--------|--------|----------|
| 0     | 0.3630 | 0.3637 | 0.000710 |
| 1     | 0.7643 | 0.7675 | 0.003238 |
| 2     | 0.8607 | 0.8631 | 0.002458 |
| 3     | 0.9136 | 0.9157 | 0.002080 |

`places=3` parity (round to 3 decimals matches).  `places=4` parity
NOT yet achieved — the residual delta comes from boundary handling
divergence: HIP uses an in-kernel clamp, CPU uses a pre-padded
mirror buffer (`PADDING_SQ_DATA_2`).  The cumulative effect compounds
across the downsampling cascade — scale 0's small per-pixel mu
delta becomes scale 3's larger numerator/denominator delta.

Tightening to places=4 is tracked as an ADR-0537 follow-up: port
the CPU pre-pad boundary into the kernel (requires either a
separate pad-and-copy kernel or a wider device staging buffer
allocated at init).

## References

- ADR-0530: HIP feature-flag promotion (un-flagged this extractor).
- ADR-0533: HIP-extractor registration sweep.
- ADR-0214: Cross-backend parity gate (places=4 target).
- AMD ROCm runtime trace: kernel launches succeed; fault address
  was in host-memory range (`0x55...`), not GPU range (`0x7f...`).
