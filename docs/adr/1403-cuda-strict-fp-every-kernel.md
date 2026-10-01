<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1403: Every CUDA feature kernel compiles without FMA contraction, and a twin spells the fused operations its reference performs

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: `cuda`, `gpu`, `numerics`, `build`, `rc3`, `fork-local`

## Context

nvcc contracts `a * b + c` into one fused multiply-add by default
(`--fmad=true`), and clang's CUDA driver does the same
(`-ffp-contract=fast`). The CPU reference build (gcc/clang, x86-64 baseline,
no `-mfma` in scalar TUs) does not contract, and since
[ADR-1367](1367-sycl-strict-fp-every-feature-tu.md) no SYCL feature TU does
either. On CUDA the answer depended on the kernel: `core/src/meson.build`
passed `--fmad=false` to six of the 21 fatbins through a per-kernel table
(`cuda_cu_extra_flags`: `speed_score`, `ssimulacra2_blur`,
`ssimulacra2_device`, `float_adm_score`, `integer_ssim_score`,
`psnr_hvs_score`), each added by the change that needed it, and left the
other fifteen on the default. Two
behaviours for one kind of kernel is a HISS-19 defect, and a kernel written
operation for operation like its CPU reference still rounded differently
unless somebody remembered to add it to the table
(`T-CUDA-FP-CONTRACT-DEFAULT-2026-09-29`).

Contraction is not always wrong. Three CPU references fuse on purpose, with
`vmaf_fmaf_exact()` in the scalar code and `fmadd` intrinsics in the SIMD
twins ([ADR-0891](0891-simd-bit-exact-round2-fmaf-libvmaf-feature-icx.md)):
`ms_ssim_decimate.c`, `ssimulacra2.c` and the SpEED matrix products. A kernel
that reproduces one of those needs the fused operation whatever the flag says.

## Decision

Every CUDA fatbin takes one flag list, `cuda_device_strict_fp_args`, defined
once between the `VMAF CUDA device strict FP policy` markers in
`core/src/meson.build`: `-Xcompiler=<host strict FP>` plus `--fmad=false`
under nvcc, and `-ffp-contract=off` under clang's CUDA driver
(`-Denable_nvcc=false`). `cuda_cu_extra_flags` stays as the place for a
kernel's other private flags and may not carry a floating-point one.
`core/test/test_strict_fp_compiler_args.py` executes the policy for both
compilers and rejects a per-kernel copy, a kernel that opts back in, a fatbin
command without the list and a second definition.

Where a fused multiply-add is part of the arithmetic a twin reproduces, the
kernel writes it as an explicit `__fmaf_rn()`, which neither flag removes.
`ssimulacra2_device.cu` and `speed_score.cu` already did;
`ms_ssim_score.cu`'s decimate now does. Division and square root need no
flag under nvcc, which defaults to `-prec-div=true -prec-sqrt=true`; nothing
passes `--use_fast_math` or `-ftz=true`. clang's CUDA driver compiles
`sqrtf()` to the approximate device square root, so a kernel that must round
like the host on both compilers writes `__fsqrt_rn()`.

`float_ms_ssim_cuda` is rewritten to its reference's arithmetic in the same
change, because the flag alone made it worse (see Consequences): the decimate
fuses each tap as `ms_ssim_decimate.c` does, the window sums are
`iqa_convolve()`'s fp32 products in an fp64 sum (carried as an exact fp32
pair), `l` / `c` / `s` keep `ssim_accumulate_default_scalar()`'s fp32
denominators and quotient, and the host rounds each per-scale mean to fp32 and
uses fp32 stabilisation constants as `iqa_ssim()` does.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the per-kernel table, add kernels as they need it | No twin's output changes | Two FP behaviours; every exact kernel must remember to join; the default disagrees with the CPU and with SYCL | HISS-19; the maintainer asked for one shared list |
| `--fmad=false` inside `cuda_flags` | One line | `cuda_flags` also feeds `cuda_args` of the host library, and the clang CUDA driver rejects `--fmad`; the per-kernel entries had already made `-Denable_nvcc=false` unbuildable for five kernels | A named device list keeps the two compilers apart and gives the test one thing to pin |
| Flag only, leave `float_ms_ssim_cuda` as it was | Smallest change | The twin gets 2x to 3x further from the CPU at 3840x2160 (5.8e-7 to 1.2e-6 max, 2.5e-7 to 7.6e-7 mean): its fp32 window sums were only close to the reference's fp64 sums because each step was fused | A policy change must not make a twin worse where the cause is known and local |
| Explicit `fmaf()` in the `float_ms_ssim_cuda` window sums (what [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md) did for `integer_vif_sycl`) | Restores the old numbers | Keeps an approximation of an fp64 sum where CUDA can compute the reference's value | The reference's arithmetic is available and makes the twin bit-identical |
| fp64 accumulators for the window sums | The reference's operations literally | 3.4 ms more per 3840x2160 frame on an RTX 4090 (10.7 to 14.1): consumer GPUs run fp64 at a small fraction of their fp32 rate | The fp32 pair gives the same frame scores at the old cost |
| Exempt a twin whose worst frame moves away from the CPU (`float_vif`, `float_motion`) | Keeps its old maximum | A third behaviour; their residual is not in the operations the flag controls (see Consequences) and the flag removes one source of difference | One behaviour; follow-up rows hold the residuals |
| **One shared device list for every fatbin, explicit FMA where the reference fuses (chosen)** | One behaviour, pinned by a test; a kernel written like its reference rounds like it; clang path builds again | Six fatbins change code; three twins' outputs move within tolerance | Chosen |

## Consequences

Measured on an RTX 4090 (sm_89, driver 615.71.09), nvcc 13.4.92, gcc 16.2.1,
release build without LTO, `master` `bd061d95a` against this change, at
`--precision max`: the Netflix 576x324 pair (48 frames), the two 1920x1080
checkerboard pairs (3 frames each) and BBB 3840x2160 (50 frames). Full tables:
[Research-1403](../research/1403-cuda-strict-fp-every-kernel.md).

- **Positive**:
  - One FP behaviour for the 21 fatbins. Fifteen keep their machine code on
    all six shipped architectures (sm_80 to sm_120): the six that already had
    the flag (`speed_score`, `ssimulacra2_blur`, `ssimulacra2_device`,
    `float_adm_score`, `integer_ssim_score`, `psnr_hvs_score`; byte-identical
    fatbins) and nine in which nvcc had nothing to fuse (`adm_dwt2`,
    `adm_cm`, `adm_csf`, `adm_csf_den`, `cambi_score`, `moment_score`,
    `motion_v2_score`, `psnr_score`, and `ssim_score`, whose roundings are
    explicit intrinsics since
    [ADR-1399](1399-cuda-float-ssim-device-decimation.md); only the PTX
    spelling `mul` becomes `mul.rn`). So `adm`, `motion`, `motion_v2`,
    `psnr`, `psnr_hvs`, `float_moment`, `cambi`, `float_adm`, `float_ssim`,
    `ssim`, `speed_chroma`, `speed_temporal` and `ssimulacra2` produce the
    same value on every frame as before.
  - `float_ms_ssim_cuda` becomes bit-identical to the CPU on every frame of
    all four fixtures (104 of 104; before 0 of 104, 6.9e-8 on the Netflix
    pair, 2.1e-6 and 4.4e-6 on the checkerboards, 5.8e-7 at 3840x2160), and
    so do its 15 `enable_lcs` outputs, which were up to 1.3e-6 off.
  - Bit-identical to the CPU on every frame of every fixture, before and
    after: `vif`, `motion`, `motion_v2`, `psnr`, `psnr_hvs`, `float_psnr`,
    `float_moment`, `float_ssim`, `cambi`, `speed_temporal`. `vif` (`filter1d`) and
    `float_psnr` change code (10 fp64 and 1 fp32 FMA fewer) and keep their
    output.
  - `ciede` gets closer on average at 3840x2160 (mean 1.17e-6 to 7.7e-7).
  - The clang CUDA path builds again. `-Denable_nvcc=false` stopped at
    configure time (`nvcc_ccbin_flags` and `nvcc_host_includes` were assigned
    in the nvcc branch only), and the per-kernel entries would have handed
    clang `--fmad=false`. With clang 22.1.8, 18 of the 19 twins give the
    nvcc build's value on every output of every frame (the four fixtures,
    22 frames of BBB; 3 952 outputs): without contraction the two compilers
    agree. `ciede` differs (device math functions). `float_ms_ssim_cuda`
    needed one more spelling for that, `__fsqrt_rn()`: clang compiles
    `sqrtf()` to the approximate device square root, nvcc to the rounded one.
- **Negative**:
  - The worst frame of two twins moves away from the CPU, inside the
    [ADR-0214](0214-gpu-parity-ci-gate.md) tolerance of 5e-5: `float_vif`
    `vif_scale3` 2.7e-5 to 3.8e-5 on the Netflix pair and 4.6e-6 to 7.0e-6
    at 3840x2160 (per-scale means move by -23% to +21%; the SYCL twin showed
    the same 2.7e-5 and 3.8e-5 on that pair before and after ADR-1367), and
    `float_motion` by up to 4% (3.0e-6 to 3.1e-6 on the Netflix pair). Neither
    is in the operations the flag controls. `float_motion` sums the per-pixel
    differences block-wise where the CPU keeps one fp32 running sum; replayed
    on the host, that running sum alone accounts for the twin's whole distance
    (`T-CUDA-FLOAT-MOTION-CPU-FLOAT-SUM-2026-10-01`). `float_vif` is not
    isolated; candidates are the device `log2f` and the order of its sums
    (`T-CUDA-FLOAT-VIF-RESIDUAL-2026-10-01`).
  - Any stored CUDA output of `ciede`, `float_motion`, `float_vif` or
    `float_ms_ssim` needs a re-run (no fork snapshot under `testdata/` is a
    CUDA output).
- **Cost**: none measurable at 3840x2160. `(t(102) - t(2)) / 100` ms per
  frame, 15 alternating pairs, host load average 9 to 16, median before and
  after with the median paired difference: `float_ms_ssim` 11.26 to 10.87
  (-0.12), `float_motion` 2.73 to 3.03 (+0.21), `float_vif` 2.53 to 2.65
  (+0.12), `ciede` 2.42 to 2.46 (+0.04), `vif` 3.42 to 3.44 (-0.07),
  `float_psnr` 2.62 to 2.61 (-0.07). Four twins whose machine code is
  identical move by -0.08 to +0.07 (`psnr_hvs`, `psnr`, `adm`,
  `speed_chroma`), and the interquartile range of every paired difference
  reaches zero. The two largest, repeated with 21 pairs of 198 frames:
  `float_motion` 2.82 to 2.89 (+0.10, quartiles -0.22 to +0.27), `float_vif`
  2.43 to 2.56 (0.00, -0.04 to +0.05), against -0.06 and -0.03 for two
  identical-code controls. No twin is near the 10% bar ADR-1367 set for an
  exemption. At 4K these twins spend their time reading and uploading the
  frame, not in the kernels.
- **Neutral / follow-ups**:
  - HIP got the same policy in parallel
    ([ADR-1407](1407-hip-strict-fp-every-kernel.md), `hip_strict_fp_args`),
    so no GPU backend that mirrors the CPU contracts by default any more.
  - The SYCL, HIP and Metal `float_ms_ssim` twins keep the arithmetic the
    CUDA twin had: `T-GPU-FLOAT-MS-SSIM-CPU-ARITHMETIC-2026-10-01`.
  - `float_ms_ssim_cuda` is bit-identical in practice, not by construction:
    the fp64 `l` / `c` / `s` sums run block-wise instead of in raster order
    and the window sums carry 48 bits instead of 53, so a per-scale mean can
    in principle land on the other side of an fp32 rounding boundary. None of
    104 frames does.

## References

- `req` (maintainer brief, 2026-10-01): "Turn contraction off for every CUDA
  feature kernel (--fmad=false through one shared flag list in
  core/src/meson.build, not per-fatbin copies), pin it with
  core/test/test_strict_fp_compiler_args.py, then measure for every CUDA twin:
  parity vs CPU before/after (which twins become bit-identical, which were
  already), and ms/frame before/after at 4K. Where an explicit FMA is part of
  a twin's CPU-matching arithmetic, keep it as an explicit intrinsic. ADR
  required (it sets the CUDA FP policy)."
- [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md),
  [ADR-0891](0891-simd-bit-exact-round2-fmaf-libvmaf-feature-icx.md),
  [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-0202](0202-float-adm-cuda-sycl.md),
  [ADR-0206](0206-ssimulacra2-cuda-sycl.md),
  [ADR-1373](1373-cuda-twin-cpu-option-parity.md),
  [ADR-1380](1380-cuda-speed-device-resident-pipeline.md),
  [ADR-1391](1391-cuda-ssimulacra2-device-resident.md),
  [ADR-1397](1397-psnr-hvs-twins-cpu-float-sum.md),
  [ADR-0139](0139-ssim-simd-bitexact-double.md),
  [ADR-0990](0990-cuda-ms-ssim-double-precision-lcs.md).
- [Research-1403](../research/1403-cuda-strict-fp-every-kernel.md).
