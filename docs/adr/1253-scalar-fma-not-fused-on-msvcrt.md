<!-- markdownlint-disable MD013 -->

# ADR-1253: Scalar references compute their fused multiply-add themselves

- **Status**: Accepted
- **Date**: 2026-09-16
- **Deciders**: lusoris
- **Tags**: `simd`, `build`, `windows`

## Context

ADR-0891 unified the fork's scalar references and their SIMD kernels on a
*single-rounded* multiply-add, and ADR-1205 records the cost of missing one
copy: the ssimulacra2 pipeline is ill-conditioned downstream, so one ULP in the
YUV→RGB matrix grew into a 2.6e-3 score delta. The scalar side spelled that
single rounding `fmaf()`.

`fmaf()` is a genuine fused multiply-add on glibc, musl and the UCRT. It is not
one on the legacy `msvcrt.dll`, and MSYS2's `MINGW64` environment — the one the
required `Windows MinGW64` lane builds in — links exactly that. There the
scalar path rounds twice while its SIMD twin rounds once.

ADR-1207's ISA-invariance gate measured it on that lane: `ssimulacra2` host-isa
`-38.376932759633718` against scalar `-38.376806310379322`, and `float_ms_ssim`
`0.86172886158050244` against `0.86172862151756147`. On a 48-frame clip the
ssimulacra2 gap is 0.37 points. Every other lane passes, because every other
lane has a correctly-rounded `fmaf`.

## Decision

Scalar references that must stay bit-exact with an FMA-using SIMD kernel call
`vmaf_fmaf_exact()` from `core/src/feature/common/fmaf_exact.h` instead of
libm's `fmaf()`. It evaluates the product and sum in `double` and rounds once
to `float`, which is the correctly-rounded fused result on every host: the
binary32 product is exact in binary64 (48 significand bits), and binary64's 53
bits are at least the 2p + 2 = 50 that makes the final rounding to binary32
innocuous.

The two call sites are `picture_to_linear_rgb` in
`core/src/feature/ssimulacra2.c` and the horizontal and vertical passes of
`core/src/feature/ms_ssim_decimate.c`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `vmaf_fmaf_exact()` — evaluate in `double`, round once (chosen) | Correct on every host and every compiler; no FMA hardware needed; no libm dependency; verified bit-identical to glibc `fmaf` and to `vfmadd213ss` over 20 million random triples | A `double` multiply and add instead of one instruction, in a per-pixel loop | — |
| Compile the scalar TUs with `-mfma` so the compiler inlines `fmaf` | One instruction; no source change | Makes the *scalar* path require FMA hardware. That path exists precisely for machines and `--cpumask` settings without it, so the binary would fault on a pre-Haswell CPU | Breaks the baseline-x86-64 contract |
| Runtime-dispatch an `-mfma` carve-out helper, fall back to libm | Keeps the single instruction where available | Reintroduces the same defect on the fallback, which is the path Windows takes; two implementations to keep in agreement | Does not fix the failing case |
| Move the MinGW lane from `MINGW64` to `UCRT64` | `fmaf` becomes correct; no source change; measured — a UCRT cross-build shows no divergence at all | Changes what the fork ships on Windows for an FP-correctness reason, and leaves the latent defect for anyone who builds against msvcrt | Wrong layer: the scalar reference should not depend on a libm's quality |

## Consequences

- **Positive**: the scalar references no longer depend on a platform libm for a
  rounding the bit-exactness contract requires. ADR-1207's gate passes on
  `Windows MinGW64`. Any future host with a non-fused `fmaf` is covered.
- **Negative**: two extra `double` conversions per call in a per-pixel loop.
  Not measurable against the surrounding work, and the SIMD path — which is
  what runs on a normal host — is untouched.
- **Neutral / follow-ups**: `grep -rn '\bfmaf\?('` over the scalar feature
  sources must stay empty for any reference with an FMA-using SIMD twin. The
  invariant is recorded in `core/src/feature/AGENTS.md` and
  `docs/rebase-notes.md`.

## References

- [ADR-0891](0891-simd-bit-exact-round2-fmaf-libvmaf-feature-icx.md) — the single-rounding contract this
  preserves. [ADR-1205](1205-ssimulacra2-fma-unification-scalar-and-gpu.md) — what one missed
  copy costs. [ADR-1207](1207-feature-isa-invariance-gate.md) — the gate that
  measured it.
- Boldo & Melquiond, *Emulation of FMA and correctly-rounded sums*, for the
  2p + 2 criterion that makes the `double` detour exact.
- Related PR: `#1425`.
- Source: `req` — "well fix it... no way around it", and the popup answer
  choosing a cross-toolchain reproduction over narrowing the gate.
