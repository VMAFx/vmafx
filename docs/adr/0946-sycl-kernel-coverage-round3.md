# ADR-0946: SYCL kernel coverage round 3 (float family + PSNR-HVS)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude
- **Tags**: sycl, test, gpu, parity, kernel-coverage

## Context

Rounds 1 (PR #351) and 2 (PR #376) closed the bulk of the integer
SYCL parity gap surfaced by the 2026-05-30 GPU-backend kernel-coverage
audit. Round 1 added `test_sycl_psnr_parity` + `test_sycl_vif_parity`.
Round 2 added `test_sycl_adm_parity` + `test_sycl_ciede_parity` +
`test_sycl_ssim_parity` + `test_sycl_ms_ssim_parity` +
`test_sycl_motion_v2_parity`. Pre-PR coverage was 12 % (~2 of 17 SYCL
extractors); after rounds 1+2 it stood at ~47 % (8 of 17). The
remaining 9 uncovered SYCL extractors include the entire float family
(`float_adm_sycl`, `float_vif_sycl`, `float_psnr_sycl`,
`float_motion_sycl`, `float_moment_sycl`), the integer PSNR-HVS
(`psnr_hvs_sycl`), the SpEED kernels (`speed_chroma_sycl`,
`speed_temporal_sycl`), and `ssimulacra2_sycl`.

The float family is the highest-value remaining target: it carries
independent kernel topologies, single-precision accumulators, and
CSF lookup tables that are not exercised by the integer-family tests
landed in rounds 1+2. A stride, sub-group-mask, or precision drift
would silently corrupt every float-VMAF model's primary feature on
Intel-Arc CHUG re-extracts.

PSNR-HVS is the next-highest-value gap: it runs a per-8x8-block DCT
with a 64-entry CSF lookup and a sub-group reduction — none of which
is exercised by any other SYCL parity test.

## Decision

Add five CPU vs. SYCL parity gates at ADR-0214 places=4 (1e-4)
tolerance under `core/test/`, gated on `enable_sycl`:

| Kernel | New test | Headline score |
| --- | --- | --- |
| `float_psnr_sycl` | `test_sycl_float_psnr_parity.c` | `float_psnr` |
| `float_adm_sycl` | `test_sycl_float_adm_parity.c` | `VMAF_feature_adm2_score` |
| `float_vif_sycl` | `test_sycl_float_vif_parity.c` | `VMAF_feature_vif_scale0_score` |
| `float_motion_sycl` | `test_sycl_float_motion_parity.c` | `VMAF_feature_motion2_score` (idx 1) |
| `psnr_hvs_sycl` | `test_sycl_psnr_hvs_parity.c` | `psnr_hvs` |

Each mirrors the round-1 / round-2 pattern: 256x144 synthetic
YUV420P fixture, per-frame XOR pattern with frame-dependent salt,
CPU + SYCL feature extractor through the public `vmaf_use_feature`
API, parity assertion via `fabs(cpu - sycl) <= 1e-4`,
skip-on-no-device via `[skip: no SYCL device]` printf.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Five per-kernel test files (chosen) | Mirrors round-2 layout; one TU per kernel keeps build parallelism and failure-isolation; reviewers can bisect a single failing kernel | Five new TUs vs. one combined | Round-2 set the precedent; combining hides which kernel regressed |
| One combined `test_sycl_round3_parity.c` | Single TU = ~250 LOC vs. ~900 LOC across five files | Failure attribution becomes harder; `meson test --run-only` granularity is lost | Loses the round-2 invariant from the AGENTS.md note in `core/src/feature/sycl/AGENTS.md` |
| Cover only the float-VMAF family (4 tests) | Tighter PR; defer PSNR-HVS to a round 4 | PSNR-HVS is the next-highest-value single gap; bundling it costs ~200 LOC | A 5-test PR still fits in the 200-800 LOC bundle target |
| Defer to `/cross-backend-diff` skill only | No new test files; relies on the existing skill | Skill is operator-invoked; CI never gates on it; regressions slip until a developer happens to run it | Same rejection rationale as round 2 |
| Cover all 9 remaining SYCL kernels in one PR | One-and-done | ~1500 LOC; SpEED and SSIMULACRA2 carry per-extractor config-arg setup not yet templated; PR exceeds the 200-800 LOC target | Defer SpEED + SSIMULACRA2 + float_moment to round 4 |

## Consequences

- **Positive**: SYCL parity coverage rises from ~47 % to ~76 %
  (13 of 17 extractors). The entire float-VMAF feature surface and
  the PSNR-HVS kernel now have CI-gated CPU↔SYCL parity at the same
  places=4 tolerance as the cross-backend gate (ADR-0214). CHUG
  re-extracts on Intel-Arc systems can be trusted against the CPU
  reference for every float-VMAF column.
- **Negative**: Five new test executables increase `enable_sycl`
  build time by ~30 s and the test runtime by ~10 s on systems
  with a SYCL device. Systems without a SYCL device run the
  skip-path and add ~0.5 s to the suite.
- **Neutral / follow-ups**: Four SYCL extractors remain uncovered
  — `speed_chroma_sycl`, `speed_temporal_sycl`, `float_moment_sycl`,
  `ssimulacra2_sycl`. Each carries per-extractor config-arg setup
  (`speed_kernelscale`, `speed_prescale`, `yuv_matrix`, etc.) that
  is not yet templated in the round-2 test scaffold. Tracked as a
  round-4 follow-up.

## References

- ADR-0214 — cross-backend numerical-parity gate (places=4 / 1e-4)
- ADR-0219 — CHUG re-extraction trusted-column invariant
- ADR-0868 — GPU-backend kernel-coverage gap audit
- ADR-0884 — SYCL kernel coverage round 2
- ADR-0108 — fork-local deep-dive deliverables rule
- PR #351 — SYCL kernel coverage round 1
- PR #376 — SYCL kernel coverage round 2
- PR #293 — SYCL init-failure cleanup leak fix
- Research digest:
  `docs/research/0946-sycl-kernel-coverage-round3-2026-05-31.md`
- Source: `req` — operator brief 2026-05-31 ("SYCL kernel coverage
  round 3 — extend beyond PRs #351 + #376").
