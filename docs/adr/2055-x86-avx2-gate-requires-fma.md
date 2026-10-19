<!-- markdownlint-disable MD013 MD060 -->
# ADR-2055: The x86 AVX2 level requires FMA

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer
- **Tags**: simd, x86, cpu, dispatch

## Context

`core/src/meson.build` compiles every AVX2 library with `-mavx -mavx2 -mfma`.
Objdump of the 21 AVX2 objects shows 103 FMA instructions in two translation
units (`ms_ssim_decimate_avx2`, dispatched on the AVX2 flag alone, and
`ssimulacra2_picture_to_linear_rgb_avx2`). `vmaf_get_cpu_flags_x86()` set the
AVX2 flag after testing BMI1, BMI2 and AVX2 in CPUID leaf 7 only and never read
the FMA bit (leaf 1 ECX bit 12), so a processor or hypervisor that reports AVX2
without FMA would fault on those kernels. Every Intel and AMD core since Haswell
and Zen has FMA3; the case is a virtual machine or emulator that masks the bit.
Row `T-CPU-AVX2-FMA-NOT-GATED-2026-10-02`.

## Decision

The AVX2 level means AVX2 + FMA + BMI1/BMI2. `vmaf_get_cpu_flags_x86()` tests
CPUID leaf 1 ECX bit 12 before it sets `VMAF_X86_CPU_FLAG_AVX2` (and so before
AVX-512, which builds on it). A CPU without FMA falls to the next lower level.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Require FMA in the AVX2 gate (**chosen**) | One check, the level then describes what the libraries are built with; covers every present and future kernel | An AVX2-without-FMA CPU (none sold) loses the other 19 AVX2 kernels | Chosen |
| Rewrite the two kernels without FMA | AVX2-without-FMA machines keep AVX2 | Changes arithmetic of a bit-exact path, and `-mfma` stays on the other objects, so the next kernel reopens the hole | Not chosen |
| A separate FMA flag for the two kernels | Precise | A flag per instruction family, no table to keep it in (RC7 builds that) | Not chosen |

## Consequences

- **Positive**: a masked-FMA guest runs the SSE paths instead of faulting.
- **Negative**: such a guest loses AVX2 speed; scores are unchanged (the SIMD paths are bit-exact against scalar).
- **Neutral / follow-ups**: `core/test/test_x86_cpu_gate.c` (mock CPUID) guards it; the RC7 CPU capability table (ADR-1490) records the feature per level.

## References

- `Q`: "Require FMA in the AVX2 gate (Recommended)" (praetor question ledger Q-014).
- [ADR-1490](1490-rc3-rc9-candidate-map-cpu-capability.md).
