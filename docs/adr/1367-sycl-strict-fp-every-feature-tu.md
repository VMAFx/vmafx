<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1367: Every SYCL feature TU compiles with contraction off and correctly rounded fp32 division and square root

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: `sycl`, `gpu`, `numerics`, `build`, `rc3`, `fork-local`

## Context

[ADR-1358](1358-sycl-speed-device-resident-linalg.md) measured that icpx's
`-fp-model=precise` still contracts `a * b + c` into one FMA inside SYCL kernel
lambdas and leaves fp32 `/` and `sqrt` approximate on the device. The SpEED
TUs, and since [ADR-1363](1363-sycl-ssimulacra2-msssim-device-resident.md)
`ssimulacra2_sycl`, therefore got `-ffp-contract=off` through a per-TU list
(`sycl_exact_fp_args` / `sycl_exact_fp_sources`) and round division and
square root in the source (`div_rn` / `sqrt_rn` in `sycl_exact_fp.h`). The
other sixteen feature TUs kept precise alone, and the backend guides described
their kernels as held "in IEEE-754 strict mode". The CPU reference build
(gcc/clang, x86-64 baseline, no `-mfma` in scalar TUs) neither contracts nor
approximates. Two behaviours for one kind of kernel is a HISS-19 defect, and
the guides promised arithmetic the device did not have
(`T-SYCL-FP-MODEL-PRECISE-CONTRACTS-2026-09-29`).

ADR-1358 also concluded that `-foffload-fp32-prec-div/-sqrt` act only on the
final image link. That was measured with relocatable device code, when device
code generation ran at the link. [ADR-1360](1360-sycl-aot-compile-time-device-codegen.md)
moved AOT code generation to compile time (`-fno-sycl-rdc`), which changes the
answer: [Research-1367](../research/1367-sycl-strict-fp-every-feature-tu.md)
measures that the pair now works per TU for the AOT images, and that the SPIR-V
JIT image is still device-linked at the link and needs the pair there.

## Decision

Under icpx every SYCL feature TU compiles with one line,
`-fp-model=precise -ffp-contract=off -foffload-fp32-prec-div -foffload-fp32-prec-sqrt`,
in that order (precise implies contraction on, so contraction-off follows it).
`core/src/meson.build` defines it once as `sycl_strict_fp_args` between the
`VMAF SYCL strict FP policy` markers; `sycl_exact_fp_args` and
`sycl_exact_fp_sources` fold into it and no TU gets a private FP list.
`sycl_dependency` carries the precision pair to every link the icpx driver
runs, so the SPIR-V JIT image of a `-Dsycl_icpx_aot_targets=` build gets it
too; a Windows MSVC build generates every image in its explicit device link
([ADR-1364](1364-windows-sycl-msvc-device-link.md)), which takes the whole line.
AdaptiveCpp keeps `-ffp-contract=off` alone. `integer_vif_sycl` writes the one
fused operation it relied on (`sigma2_sq - g * sigma12`, an fp64 expression on
the CPU) as `sycl::fma`. `div_rn` / `sqrt_rn` stay where ADR-1358 and ADR-1363
put them. The backend guides state what the line guarantees: fp32 `+ - * /
sqrt` round as on the CPU, with no contraction and subnormals kept; scores are
still not bit-identical where a twin uses transcendentals, another reduction
order, fp32 in place of a CPU fp64 expression, or a different formula.
`test_sycl_fp_arith_contract` checks the device arithmetic on hardware.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the per-TU list (`sycl_exact_fp_sources`) | No other twin's output changes | Two FP behaviours for one kind of kernel; every new exact TU must remember to join the list; the guides stay wrong for sixteen TUs | HISS-19; the maintainer asked for one behaviour |
| Contraction off for every TU, no precision pair | One flag; no division cost | Device `/` and `sqrt` still differ from the host in 29% and 8% of random operands, so every twin that divides keeps that difference (a contraction-only build was not measured per twin) | Leaves half of the finding open |
| Precision pair on the link only (ADR-1358's reading) | One place | Since ADR-1360 the AOT images are finished at compile time; measured: a link-only pair leaves them approximate (1 219 433 of 4 194 304 divisions differ) | Does not reach the images that run on every listed device |
| `div_rn` / `sqrt_rn` in every kernel's source | Exact whatever the flags; fast path cheap | Every `/` and `sqrt` in 20 TUs rewritten and every future one remembered; a second implementation of what the compiler flag gives | HISS-19; the flag covers it |
| Exempt a twin that gets slower | Keeps its old cost | No twin crossed the ADR-1367 bar (> 10% slower at 4K with no parity gain), see Consequences | Nothing to exempt |
| **One strict line for every feature TU, pair also on the link (chosen)** | One behaviour; the guides can state a guarantee; the device test enforces it on both image paths | Changes the output of nine twins (each still inside its ADR-0214 tolerance); `-Woverriding-option` note on every SYCL TU compile | Chosen |

## Consequences

- **Positive**: every fp32 `+ - * / sqrt` in a SYCL feature kernel rounds as on
  the CPU, on both devices and both image paths; `test_sycl_fp_arith_contract`
  (1 048 576 random operands and 2 197 boundary combinations, subnormal
  results included) passes on the Arc B580 and the UHD 770 in the AOT build
  and in a `-Dsycl_icpx_aot_targets=` build, and fails on the JIT path when
  the link pair is removed (305 000 divisions differ). Per-twin max abs diff
  against `--backend cpu` at `--precision max`, before -> after, identical on
  both GPUs unless noted (576x324: Netflix pair, 48 frames; 4K: BBB
  3840x2160, 22 frames):
  - better: `float_adm` 2.50e-5 -> 2.53e-6 and 1.12e-6 -> 1.97e-7; `ssim`
    1.40e-8 -> 7.40e-9 (UHD 770 1.47e-8 -> 7.15e-9) and 7.76e-8 -> 5.59e-8;
    `float_ssim` 3.10e-7 -> 1.47e-7; `ciede` 1.18e-5 -> 1.14e-5 and 9.71e-5 ->
    4.53e-5; `vif` 3.87e-7 -> 3.49e-7 at 576x324.
  - unchanged output: `adm`, `motion`, `motion_v2`, `float_psnr`, `psnr`,
    `float_moment`, `ssimulacra2`, `cambi`, `speed_chroma`, `speed_temporal`,
    and the default model's features (bit-identical twins stay so).
  - worse max, inside tolerance: `float_vif` 2.71e-5 -> 3.81e-5 at 576x324
    (4K 3.18e-6 -> 3.14e-6); `float_ms_ssim` 2.84e-7 -> 4.69e-7 at 4K (mean
    halves, 1.36e-7 -> 6.75e-8); `vif` 1.22e-7 -> 1.49e-7 at 4K (mean
    2.79e-8 -> 2.68e-8); `float_motion` 3.05e-6 -> 3.09e-6 at 576x324 (4K
    2.67e-5 -> 2.29e-5). Their residual drift is not in the operations the
    line controls: `float_vif` keeps a systematic 3.4-3.6e-6 offset at scale
    0, most likely from `sycl::log2` and the CPU's fp64 `1 + x / y` (not
    isolated), and the others sum in a different order or approximate fp64
    terms in fp32; the change moves which frame is worst. `integer_vif_sycl` without the explicit `fma` was 6x
    worse on the Netflix pair (mean 6.29e-8 -> 3.75e-7), which is why its
    fused operation is now written out.
  - ADR-0214 gate (`cross_backend_parity_gate.py --backends cpu sycl`,
    Netflix pair, one feature per run): the 15 runnable cells pass on both
    GPUs before and after; the `cambi` and `motion` cells abort on stale
    metric names in the gate (`T-CI-PARITY-GATE-STALE-METRIC-KEYS-2026-09-29`).
  - 4K cost on the B580, in-memory harness, median of 5-7 runs: every twin
    within -8% to +3.1%, inside the spread of the unchanged kernels, so no
    twin is more than 10% slower. The CLI method was too noisy on the shared
    host to resolve 10%. See
    [Research-1367](../research/1367-sycl-strict-fp-every-feature-tu.md) §Cost.
- **Negative**: nine twins' outputs change, so any stored per-build SYCL
  output comparison needs a re-run (no fork snapshot under `testdata/` is a
  SYCL output). icpx prints `-Woverriding-option` ("overriding
  '-ffp-model=precise' option with '-ffp-contract=off'") for every SYCL TU
  compile: 60 instead of 12; it is the documented, intended override, the same
  one the icx x86 strict libraries print 43 times. The Windows MSVC device
  link (ADR-1364, merged after this ADR was first measured) passes the line to
  every Windows image, native and SPIR-V; no Windows GPU run has checked it.
- **Neutral / follow-ups**: `div_rn` / `sqrt_rn` are now redundant on icpx
  but still exact; their slow path could use `/` now that it is correctly
  rounded (unmeasured). CUDA (`--fmad=true` default) and HIP
  (`-ffp-contract=fast` default) still contract in all but two kernels each:
  `T-CUDA-FP-CONTRACT-DEFAULT-2026-09-29`, `T-HIP-FP-CONTRACT-DEFAULT-2026-09-29`.
  On the Arc A380 (`dg2-g11`, measured 2026-09-30 under the xe kernel driver)
  `test_sycl_fp_arith_contract` passes on the SPIR-V JIT image and on a
  `dg2-g11` AOT image. The SYCL suite fails the same 16 tests there on master
  and with this change: under that driver, kernels that use scratch memory
  return wrong values. `float_moment` twins are never
  selected by `--feature float_moment`: `T-CLI-FLOAT-MOMENT-NO-TWIN-2026-09-29`.

## References

- `req` (maintainer brief, 2026-09-29): "Decide per TU whether all SYCL feature
  TUs get the strict-fp args (default expectation: yes, all of them — one
  behaviour, one flag set, HISS-19; reuse the existing variable, do not add a
  second one)"; "If a twin gets materially slower (>10%) for no parity gain,
  document the exception with numbers rather than silently excluding it."
- [ADR-1358](1358-sycl-speed-device-resident-linalg.md),
  [ADR-1363](1363-sycl-ssimulacra2-msssim-device-resident.md),
  [ADR-1360](1360-sycl-aot-compile-time-device-codegen.md),
  [ADR-0202](0202-float-adm-cuda-sycl.md), [ADR-0214](0214-gpu-parity-ci-gate.md),
  [ADR-0220](0220-sycl-fp64-fallback.md), [ADR-0407](0407-adaptivecpp-second-sycl-toolchain.md).
- [Research-1367](../research/1367-sycl-strict-fp-every-feature-tu.md).
