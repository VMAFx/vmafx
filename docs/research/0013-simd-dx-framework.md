# Research-0013: SIMD DX framework — audit + NEON bit-exactness port

- **Status**: Active
- **Workstream**: [ADR-0140](../adr/0140-simd-dx-framework.md)
- **Last updated**: 2026-04-21

## Question

After [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md) and
[ADR-0139](../adr/0139-ssim-simd-bitexact-double.md), the fork holds
two auditable bit-exactness patterns that every future SIMD kernel
must preserve:

1. Single-rounded `float * float` → widen to `double` → `double +=`
   (no FMA).
2. Per-lane scalar-double reduction for kernels whose scalar C loop
   type-promotes via a `double` literal (`2.0 * ...`).

Both patterns were written longhand in AVX2 / AVX-512 TUs. We wanted
to answer two questions at once:

- **Q1.** What is the *minimum* fork-internal scaffolding that lets
  the next SIMD kernel reuse these patterns without re-deriving them
  (and without importing Highway / simde / xsimd, which the fork's
  SIMD policy rules out — see
  [memory `feedback_simd_dx_scope.md`](../../.claude/projects/-home-kilian-dev-vmaf/memory/feedback_simd_dx_scope.md))?
- **Q2.** Does NEON's current SSIM / convolve SIMD on aarch64 match
  the same bit-exactness bar as AVX2 / AVX-512 after PR #18 + PR #76,
  or does it carry ADR-0138 / ADR-0139-class drift that was never
  surfaced because CI has no aarch64 runner?

## Sources

- **Scalar references**:
  [`_iqa_convolve`](../../core/src/feature/iqa/convolve.c)
  and
  [`ssim_accumulate_default_scalar`](../../core/src/feature/iqa/ssim_tools.c).
- **Existing AVX2 / AVX-512 SIMD**:
  [`ssim_avx2.c`](../../core/src/feature/x86/ssim_avx2.c) /
  [`ssim_avx512.c`](../../core/src/feature/x86/ssim_avx512.c) /
  [`convolve_avx2.c`](../../core/src/feature/x86/convolve_avx2.c) /
  [`convolve_avx512.c`](../../core/src/feature/x86/convolve_avx512.c)
  as of commit `6db63310` (PR #76 HEAD).
- **Existing NEON SIMD**:
  [`ssim_neon.c`](../../core/src/feature/arm64/ssim_neon.c)
  as of commit `be1f74d1` (PR #76 pre-DX).
- **Upstream baseline**: Netflix/vmaf `origin/master` has **no** SSIM
  SIMD on *any* ISA. `convolve_neon` doesn't exist upstream — the
  existing `iqa_convolve_avx2` / `iqa_convolve_avx512` are fork-only.
  First NEON SSIM touch is fork commit `81fcd42e`.
- **Measurement (aarch64)**:
  - `aarch64-linux-gnu-gcc` cross toolchain.
  - `qemu-aarch64-static -L /usr/aarch64-linux-gnu` exe_wrapper (meson
    cross-file [`build-aux/aarch64-linux-gnu.ini`](../../build-aux/aarch64-linux-gnu.ini)).
  - `vmaf --cpumask 255` — all SIMD blocked (scalar path).
  - `vmaf` — native (NEON active).
  - `--precision max` forces `%.17g` IEEE-754 round-trip.
- **Related ADRs**:
  [ADR-0125](../adr/0125-ms-ssim-decimate-simd.md) (bit-exactness
  ground rule),
  [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md) /
  [ADR-0139](../adr/0139-ssim-simd-bitexact-double.md) (x86
  patterns),
  [ADR-0140](../adr/0140-simd-dx-framework.md) (this workstream's
  decision).

## Findings

### SIMD-gap inventory (9 gaps; shaped PR #A + PR #B scope)

Audit of `core/src/feature/` against the fork's existing
AVX2 / AVX-512 / NEON coverage:

| # | Feature | AVX2 | AVX-512 | NEON | Gap class |
| --- | --- | --- | --- | --- | --- |
| 1 | `iqa_convolve` (SSIM / MS-SSIM inner) | ✓ | ✓ | **missing** | **New NEON TU** |
| 2 | `ssim_accumulate` (per-lane reduce) | ✓ | ✓ | present but ADR-0139 drift | **NEON bit-exactness port** |
| 3 | `ssimulacra2` | **missing** | **missing** | **missing** | **New 3-ISA** |
| 4 | `motion_v2` | present (upstream) | present (upstream) | **missing** | **New NEON TU** |
| 5 | `vif_statistic` | present (upstream) | **missing** | present (partial) | **AVX-512 + NEON gap** |
| 6 | `float_ansnr` | **missing** | **missing** | **missing** | **New 3-ISA** |
| 7 | `moment` | **missing** | **missing** | **missing** | **New 3-ISA** |
| 8 | `luminance_tools` | **missing** | **missing** | **missing** | **New 3-ISA** |
| 9 | DX macros + scaffold skill | n/a | n/a | n/a | **Tooling** |

Gap #9 is the *tool* that makes gaps #1-#8 tractable. Demonstrated in
PR #A on gaps #1 + #2 (real code, both demos shipped). Gaps #3-#8 are
PR #B scope.

### Drift audit under QEMU (ADR-0139 check on NEON)

Ran scalar vs NEON on the Netflix 576×324 pair + 1920×1080
checkerboard (1-px) pair, cross-compiled with
`aarch64-linux-gnu-gcc`, run under `qemu-aarch64-static`.

Before the NEON per-lane-scalar-double fix:

```text
Netflix pair, frame 0:  float_ssim    0.92502313852310181 (scalar)
                                      0.92502307891845703 (NEON)     Δ ≈ 6e-08
Netflix pair, frame 4:  float_ssim    0.85594284534454346 (scalar)
                                      0.85594278573989868 (NEON)     Δ ≈ 6e-08
Netflix pair, frame 7:  float_ms_ssim 0.95786428224536158 (scalar)
                                      0.95786428986614292 (NEON)     Δ ≈ 8e-09
```

Root cause: same as ADR-0139 on AVX2 / AVX-512. Scalar computes
`lv = (2.0 * rm * cm + C1) / ...` in `double`
(because the `2.0` C literal promotes its float operands to `double`
and `lv` is `double`). The NEON code kept everything in
`float32x4_t`, then widened the *final* `l * c * s` to `double` — a
different numerical pipeline by construction.

After applying the ADR-0139 fix (per-lane scalar-double reduction
using `SIMD_ALIGNED_F32_BUF_NEON` + `SIMD_LANES_NEON`), the XML diff
collapses to the `<fyi fps="..."/>` line only (runtime metadata).

### Convolve NEON port — bit-exact by construction

New NEON `iqa_convolve_neon` built on top of
`SIMD_WIDEN_ADD_F32_F64_NEON_4L`. Matches scalar under
`FLT_EVAL_METHOD == 0`:

- 11-tap Gaussian (odd kernel, `kw_even == 0`): exact.
- 8-tap box (even kernel, `kw_even == 1`): exact.
- Sizes: 11×11 / 12×12 / 19×19 / 20×20 / 25×25 / 33×17 / 61×41 /
  576×324 / 1920×1080 / 8×8 / 16×16 / 21×13.

`test_iqa_convolve` extended to cover aarch64 under QEMU — now
runs in the `arm64` / `aarch64` arch filter alongside the existing
`x86_64` / `x86` coverage.

### Cross-compile infrastructure

Added [`build-aux/aarch64-linux-gnu.ini`](../../build-aux/aarch64-linux-gnu.ini)
so local `meson setup --cross-file=...` works. Host requires
`aarch64-linux-gnu-gcc` + `qemu-user-static` + an aarch64 sysroot
under `/usr/aarch64-linux-gnu`. QEMU is invoked with
`-L /usr/aarch64-linux-gnu` so the dynamic linker
`/lib/ld-linux-aarch64.so.1` resolves.

One pre-existing test-gate bug surfaced during this audit:
[`core/test/dnn/test_cli.sh`](../../core/test/dnn/test_cli.sh)
invokes `$VMAF_BIN` directly from bash. Meson's `exe_wrapper` is not
applied to env-provided binaries inside shell scripts, and the host's
binfmt_misc entry doesn't know about the aarch64 sysroot prefix — so
qemu fails to load the aarch64 dynamic linker. Fixed by gating the
`test_cli` registration on `not meson.is_cross_build()` in
[`core/test/dnn/meson.build`](../../core/test/dnn/meson.build).
Unrelated to the NEON work; fix is in-scope for PR #A because PR #A
introduces the aarch64 cross-compile lane.

## Decision emitted

See [ADR-0140](../adr/0140-simd-dx-framework.md). Two-part framework:
a header (`simd_dx.h`) with ISA-specific macros + an upgraded
`/add-simd-path` skill that scaffolds new SIMD TUs from a short
kernel-spec declaration.

## Dead ends / not chosen

- **Cross-ISA portability layer (Highway / simde / xsimd).** Writes
  once, runs everywhere, but hides the ISA-specific bit-exactness
  rules (FMA availability, rounding mode, lane ordering) inside the
  abstraction. The fork's SIMD policy (user memory
  `feedback_simd_dx_scope.md`) explicitly rules this out.
- **Generic cross-ISA `SIMD_WIDEN_ADD_F32_F64(acc, a, b)` macro.** A
  single name behind `#if __AVX2__` / `__AVX512F__` / `__ARM_NEON`
  dispatch would read cleaner in-line, but it conceals which
  intrinsics a given line emits and breaks reviewer intuition on
  bit-exactness trade-offs. ISA-suffixed names (`_AVX2_4L`,
  `_AVX512_8L`, `_NEON_4L`) let a reviewer audit the intrinsics
  without opening the header.
- **Codegen-only skill (no header).** The skill alone would have cut
  the per-TU bootstrap cost, but the bit-exactness patterns would
  stay copy-pasted and easy to lose silently on the next port.
- **Dedicated `ssim_accumulate_*` unit test (AVX2 + AVX-512 + NEON
  uniformly).** Good idea but broader than PR #A — spans 3 ISAs and
  would inflate review surface. Deferred into PR #B.

## Status

Active. Will be closed when PR #B lands and the macros have been
consumed by ≥3 additional kernels (the ssimulacra2 + motion_v2 +
vif_statistic set).
