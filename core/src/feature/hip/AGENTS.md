# HIP Feature Extractors — Invariant Notes

## Deleted orphan/dead TUs (ADR-0546)

The following files were removed from this directory by ADR-0546
(`chore/hip-cuda-orphan-tu-cleanup`, 2026-05-18):

- `adm_hip.c` — defined `vmaf_hip_adm_{init,run,destroy}` stubs
  (`init` returned 0, `run` returned -ENOSYS); no `VmafFeatureExtractor`
  registration; zero callers in the repo. API-level HIP ADM is
  covered by `integer_adm_hip.c` (`vmaf_fex_integer_adm_hip`).
- `motion_hip.c` — same pattern; `vmaf_hip_motion_{init,run,destroy}`;
  covered by `integer_motion_hip.c` and `float_motion_hip.c`.
- `vif_hip.c` — same pattern; `vmaf_hip_vif_{init,run,destroy}`;
  covered by `integer_vif_hip.c` and `float_vif_hip.c`.
- `feature_hip.h` — forward-declared only the above three triplets;
  removed with the last of its consumers.

Also removed from `libvmaf/src/feature/hip/`:

- `integer_ciede_hip.c` — duplicate of `ciede_hip.c`; both defined
  `vmaf_fex_ciede_hip`. Only `ciede_hip.c` is in `hip/meson.build`.
- `integer_moment_hip.c` — duplicate of `float_moment_hip.c`; both
  defined `vmaf_fex_float_moment_hip`. Only `float_moment_hip.c` is
  in `hip/meson.build`.

And from `libvmaf/src/feature/cuda/`:

- `float_ssim_cuda.c` — stale copy superseded by `integer_ssim_cuda.c`;
  both defined `vmaf_fex_float_ssim_cuda`. Only `integer_ssim_cuda.c`
  is in `libvmaf/src/meson.build`. The newer TU adds `enable_chroma`
  and other improvements missing from the orphan copy.

Do not re-add any of these files without first consulting ADR-0546.

## Memory copy direction enum discipline

Every `hipMemcpy*` call's direction enum **must match the actual memory placement** of source and destination pointers:

- `hipMemcpyHostToDevice`: source is host-accessible (CPU pointer), destination is device-side
- `hipMemcpyDeviceToHost`: source is device-side, destination is host-accessible (CPU or pinned)
- `hipMemcpyDeviceToDevice`: source and destination are both device-side

Mismatches are undefined behavior on some HIP runtimes and may silently corrupt results or trigger runtime faults.

**Established patterns:**
- Picture planes arrive from the VMAF pipeline as CPU-side `VmafPicture` structs with `data[0..2]` pointers (host memory). Copying these into device-allocated staging buffers requires `hipMemcpyHostToDevice`.
- Readback buffers allocated via `hipHostMalloc` in `src/hip/kernel_template.c` are host-pinned memory, safe to use with `hipMemcpyDeviceToHost` for kernel output collection.

See PR #[TBD] / ADR-[TBD] for the discovery and fix of `integer_psnr_hip.c` lines 316/322 (2026-05-16 GPU audit).

## Kernel-arg pattern for `hipModuleLaunchKernel` pointer parameters (ADR-0537)

When a `__global__` kernel takes a pointer parameter (e.g.
`const uint16_t *vif_filt_dev`), the corresponding entry in the host
`void *args[]` array must be the **address of a variable that holds
the device pointer** — NOT the device pointer value itself, and NOT
the address of host memory.

```c
/* CORRECT — &dev_ptr_var points to the variable storing the device ptr */
void *dev_ptr = s->some_dev_malloc;
void *args[] = { /* …, */ &dev_ptr, /* … */ };
hipModuleLaunchKernel(func, …, args, NULL);

/* WRONG — passes a host address that the GPU will dereference */
void *args[] = { /* …, */ (void *)host_static_array, /* … */ };

/* WRONG — passes the pointer value into the position where the HIP
 * runtime expects the address-of-pointer */
void *args[] = { /* …, */ s->some_dev_malloc, /* … */ };
```

The pre-ADR-0537 `integer_vif_hip.c` had the second form for the
filter table parameter, which the AMD GPU dereferenced and faulted
on with "Memory access fault by GPU node-1 ... Reason: Page not
present or supervisor privilege" — on the first frame, before any
score had been produced.

## Static const tables must be uploaded to device memory (ADR-0537)

If a host-side `static const` array (e.g. `vif_filter1d_table[4][18]`
from `feature/integer_vif.h`) needs to be readable from a HIP kernel,
allocate a device buffer at init time and `hipMemcpy(...,
hipMemcpyHostToDevice)` the table contents once.  Don't try to pass
the host address into the kernel via `args[]` — it WILL fault.

The cost is ~150 bytes one-shot at init, amortised across the
extractor's lifetime.  Established precedent: ADR-0537 in
`integer_vif_hip.c::init_fex_hip()`.

## Kernel name-suffix convention does NOT encode filter half-width (ADR-0537)

The CUDA-port kernel-name suffixes like `filter1d_8_vertical_kernel_uint32_t_17_9`
or `filter1d_16_vertical_kernel_uint2_3_0_3` encode `(fwidth_0, fwidth_1, scale)`
— the *full filter widths* for the main filter and the rd downsample filter,
plus the scale index.  They are NOT half-widths.

Correct filter half-widths come from `vif_filter1d_width[scale] / 2`:

| Scale | `fwidth` | `half_width` |
|-------|----------|--------------|
| 0     | 17       | 8            |
| 1     | 9        | 4            |
| 2     | 5        | 2            |
| 3     | 3        | 1            |

The pre-ADR-0537 `integer_vif/vif_statistics.hip` used `HALF = 9 / 5 / 3 / 0`
(parsed from the suffix), which read 19 / 11 / 7 / 1 filter coefficients per
output pixel from an 18-entry table — out-of-bounds reads.

## Scalar-per-thread is the correctness baseline; templated tiled is the perf goal

When porting a CUDA twin to HIP, write the kernel scalar-per-thread first
(no shared-memory tiling, no warp reductions) and confirm cross-backend
parity at `places=3` (or better) on the Netflix golden pair *before*
porting the perf optimisations.  HIP wavefront sizes differ between
RDNA (32) and GCN/CDNA (64), so the warp-reduce path needs its own tuning
even after the scalar kernel is bit-exact.

Established precedent: ADR-0537 ports `integer_vif/vif_statistics.hip`
scalar-per-thread (~540 lines vs the CUDA twin's ~850), accepts a
~5–10× perf regression vs CUDA in exchange for a verifiable kernel
surface.  Perf optimisation deferred to a follow-up ADR.

## HSACO symbol naming — kernel keys must match the host-TU consumer (ADR-0539)

When a HIP host TU references a kernel module via
`hipModuleLoadData(..., <name>_hsaco)`, the `hip_kernel_sources` meson
key MUST be exactly `<name>` — the `xxd -i -n <name>_hsaco` step inside
the meson custom_target derives the symbol from that key.  Two
gotchas:

1. **Distinct host TUs that consume different kernels must use
   distinct meson keys, even when the underlying `.hip` filename is
   `moment_score.hip`** for both.  Compare:

   ```meson
   # float_moment_hip.c consumes `moment_score_hsaco`
   'moment_score' : feature_src_dir + 'hip/float_moment/moment_score.hip',
   # integer_moment_hip.c consumes `integer_moment_score_hsaco`
   'integer_moment_score' : feature_src_dir + 'hip/integer_moment/moment_score.hip',
   ```

   The two `.hip` files contain different kernel entry points
   (`calculate_float_moment_*` vs `calculate_integer_moment_hip_kernel_*`)
   and are NOT interchangeable.

2. **A missing meson registration produces an undefined-reference link
   error** for `<name>_hsaco`, NOT a runtime `-ENOSYS`.  If you see
   such a link failure, either register the kernel (preferred) or add
   a weak stub in `hip_hsaco_stubs.c` (per ADR-0536 — only for kernels
   that can't yet compile standalone via `hipcc --genco`).
## Remove the weak HSACO stub the moment a real .hip lands (ADR-0539)

When a `.hip` kernel under `feature/hip/<extractor>/` becomes
standalone-buildable and you register it in `hip_kernel_sources` in
`libvmaf/src/meson.build`, **also delete its matching
`VMAF_HSACO_WEAK_STUB(<extractor>_score_hsaco)` line from
`hip_hsaco_stubs.c` in the same PR.**  Leaving the stub creates two
definitions of the same symbol — a strong xxd-embedded blob and a weak
1-byte fallback — which the linker resolves to the strong one but at
the cost of `-Wlto-type-mismatch` warnings on every build.  The user
direction is "no stubs anywhere" once a real kernel exists.

Pattern (ADR-0539 example for `float_vif_score`):
1. Confirm the `.hip` source compiles via `hipcc --genco` in the
   container (`ninja -C <build> src/<name>.hsaco`).
2. Remove the `VMAF_HSACO_WEAK_STUB(<name>_hsaco)` line from
   `hip_hsaco_stubs.c`.  Leave a one-line comment citing the ADR so the
   reviewer sees why the slot is gone.
3. Rebuild with `enable_hipcc=true` and grep the ninja output for
   warnings referencing the symbol — none should remain.
## IEEE-strict kernels go in `hip_cu_extra_flags` (ADR-0539)

When a HIP kernel relies on IEEE-754 add/mul ordering — for example any
recursive IIR (the SSIMULACRA2 FastGaussian cascade), angle-flag
reductions, or numerically-sensitive variance / covariance combines —
add an entry to the `hip_cu_extra_flags` dict in
`libvmaf/src/meson.build` with `['-ffp-contract=off']` (or richer flag
list as needed).  hipcc / amdclang++ default to `-ffp-contract=fast` on
the device side, which silently fuses `n2 * sum - d1 * prev` patterns
into FMAs and shifts the recursion past places=2 vs the CPU / Vulkan
`precise` twin.  Mirrors the CUDA `cuda_cu_extra_flags` dict in the same
file.  Current entries: `ssimulacra2_blur`.  Rebase invariant: when
porting a new CUDA kernel that lists `--fmad=false` /
`-ffp-contract=off` in `cuda_cu_extra_flags`, add the matching HIP
entry in the same PR.
## Per-thread atomicAdd replaces CUDA per-warp `__shfl_down_sync` reduce (ADR-0539)

The CUDA twin's `cuda_helper.cuh::warp_reduce` hard-codes
`warpSize == 32` in the `__shfl_down_sync(0xffffffff, …)` mask.  AMD
wavefronts are **64 wide** on every GCN / CDNA / RDNA target we ship to
(gfx906 / gfx90a / gfx10 / gfx11), so the CUDA shuffle pattern is
incorrect on AMD even when `__shfl_down_sync` is available.

**Established pattern** when porting a CUDA kernel that ends in
`warp_reduce(accum) + per-warp atomicAdd`:

1. Drop the warp reduce entirely.
2. Have **every thread** call `atomicAdd((uint64_cu *)&accum_global[band], lane_value)`.
3. Bit-exact w.r.t. the CUDA twin since unsigned 64-bit integer addition
   is associative and commutative — only the reduction *order* changes.
4. Works on every AMD wavefront width without `#ifdef`-ing per arch.

`atomicAdd` on `unsigned long long` is native on gfx90a / gfx10 / gfx11
and falls back to a CAS loop on older GCN — the HIP runtime handles the
arch selection.

Precedents: `integer_vif/vif_statistics.hip` (ADR-0537),
`integer_adm/adm_csf_den.hip` + `integer_adm/adm_cm.hip` (ADR-0539).

## ADM `_hsaco` weak-stub slots have been removed (ADR-0539)

The `hip_hsaco_stubs.c` weak fallbacks for `adm_dwt2_hsaco`,
`adm_csf_hsaco`, `adm_csf_den_hsaco`, `adm_cm_hsaco` have been
**removed** — the four `.hip` kernels now build standalone via
`hipcc --genco` (registered in `libvmaf/src/meson.build::hip_kernel_sources`)
and their xxd-embedded strong symbols supply the blobs the host TU loads.

If a future ADM PR re-introduces a CUDA-only helper into one of the
four kernels (re-breaking the standalone build), do NOT re-add a weak
stub — fix the kernel.  Falling back to weak stubs silently degrades
HIP ADM to CPU at runtime (the `hipModuleLoadData` call returns
non-zero on an empty blob and the extractor returns `-ENOSYS` from
`init()`), which is what the user directive "no stubs anywhere"
explicitly rules out.

The `VMAF_HSACO_WEAK_STUB` macro in `hip_hsaco_stubs.c` is retained
as a documented pattern for in-progress ports of *new* extractors;
it is currently used by zero extractors.

## AdmBufferHip struct-by-value kernel parameters — P1 known issue (Research-0755)

`AdmBufferHip` (defined in `integer_adm_hip.h:70–96`) is a ~272-byte struct
containing 6 DWT band sub-structs (each 4 device pointers) plus 8 additional
device-pointer fields.  It is currently passed by value in multiple `__global__`
kernel signatures in `integer_adm/adm_csf.hip` and `integer_adm/adm_cm.hip`.

This mirrors the PR #93 F3 finding on the CUDA side.  Consequences:
- Every GPU thread's stack receives a full 272-byte copy via the kernel-argument
  buffer path.  On RDNA/GCN this adds measurable argument-passing overhead.
- Structs this large risk hitting the HIP/AMDDriver kernel-argument limit (varies
  per target; typically 1024–4096 bytes total across all args).

**Recommended fix**: replace `AdmBufferHip buf` parameters with
`const AdmBufferHip * __restrict__ buf` (pass a pointer to a device-side copy
of the struct).  No correctness impact — only the passing convention changes.

Until fixed: do NOT add new `__global__` parameters of type `AdmBufferHip` by
value.  Any new ADM kernel should take a pointer.

## extern "C" macro-instantiation pattern is correct (Research-0755)

Several ADM kernel files (`adm_csf.hip`, `adm_csf_den.hip`, `adm_dwt2.hip`)
define `__global__` kernel bodies inside `#define` macros, then instantiate
those macros inside an `extern "C" { }` block.  This is correct: the C++
preprocessor expands the macro at the point of instantiation (inside
`extern "C"`), so the resulting function definition is unmangled and
`hipModuleGetFunction` name lookups work.  This is NOT an `extern "C"` gap.

The pattern is load-bearing.  Do not "fix" it by adding an additional
`extern "C"` declaration inside the macro body — that would create a nested
`extern "C"` which is legal in C++ but redundant and confusing to reviewers.
