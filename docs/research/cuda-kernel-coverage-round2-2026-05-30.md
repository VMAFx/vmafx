<!-- markdownlint-disable MD018 MD060 -->
# CUDA-kernel parity-test coverage — round 2 audit (2026-05-30)

Status: complete (paired with ADR-0886 and the round-2 PR).

## Why this digest exists

The 2026-05-30 round-1 PR (#351 / ADR-0868) added two CUDA parity tests
(`test_cuda_psnr_parity` and `test_cuda_ciede_parity`). User asked for
round 2 — extend coverage by 4–5 more kernels, avoiding overlap with PR
#351 (psnr_cuda, ciede_cuda) and PR #289 (CUDA PTX unload sweep, no
test files). This digest documents the audit that picked the five
round-2 kernels and the rationale for the omitted ones.

## CUDA extractor inventory (master @ 387839eacf)

Source enumeration (`find core/src/feature/cuda -name '*.c'` + `grep
.name`):

| Extractor                | C-source                            | Kernel(s) (.cu)                                        | Notes |
|--------------------------|-------------------------------------|--------------------------------------------------------|-------|
| `adm_cuda`               | `integer_adm_cuda.c`                | `integer_adm/{adm_dwt2,adm_decouple,adm_csf,adm_csf_den,adm_cm}.cu` | Default-model lineage |
| `ciede_cuda`             | `integer_ciede_cuda.c`              | `integer_ciede/ciede_score.cu`                         | **Covered by round 1 (PR #351)** |
| `psnr_cuda`              | `integer_psnr_cuda.c`               | `integer_psnr/psnr_score.cu`                           | **Covered by round 1 (PR #351)** |
| `motion_cuda`            | `integer_motion_cuda.c`             | `integer_motion/motion_score.cu`                       | motion3 covered by `test_cuda_motion3_parity` (round-0) |
| `motion_v2_cuda`         | `integer_motion_v2_cuda.c`          | `integer_motion_v2/motion_v2_score.cu`                 | neg-model lineage; SAD reduction uncovered |
| `vif_cuda`               | `integer_vif_cuda.c`                | `integer_vif/filter1d.cu`                              | Covered by `test_integer_vif_cpu_cuda_parity` (round-0) |
| `cambi_cuda`             | `integer_cambi_cuda.c`              | `integer_cambi/cambi_score.cu`                         | neg + 4k model lineage |
| `psnr_hvs_cuda`          | `integer_psnr_hvs_cuda.c`           | `integer_psnr_hvs/psnr_hvs_score.cu`                   | HVS DCT path |
| `integer_ssim_cuda`      | `ssim_cuda.c`                       | `integer_ssim/{ssim_score,integer_ssim_score}.cu`      | Standard SSIM companion |
| `float_ssim_cuda`        | `integer_ssim_cuda.c` (variant)     | shared with integer_ssim                               | Lower priority |
| `float_ms_ssim_cuda`     | `integer_ms_ssim_cuda.c`            | `integer_ms_ssim/ms_ssim_score.cu`                     | Lower priority (no default-model dep) |
| `float_adm_cuda`         | `float_adm_cuda.c`                  | `float_adm/float_adm_score.cu`                         | Lower priority (integer variant covers ADM family) |
| `float_motion_cuda`      | `float_motion_cuda.c`               | `float_motion/float_motion_score.cu`                   | Lower priority |
| `float_psnr_cuda`        | `float_psnr_cuda.c`                 | `float_psnr/float_psnr_score.cu`                       | Lower priority |
| `float_vif_cuda`         | `float_vif_cuda.c`                  | `float_vif/float_vif_score.cu`                         | Lower priority |
| `float_moment_cuda`      | `integer_moment_cuda.c`             | `integer_moment/moment_score.cu`                       | Diagnostic only; no model dep |
| `speed_chroma_cuda`      | `speed_chroma_cuda.c`               | `speed/speed_score.cu`                                 | No model dep |
| `speed_temporal_cuda`    | `speed_temporal_cuda.c`             | `speed/speed_score.cu`                                 | No model dep |
| `ssimulacra2_cuda`       | `ssimulacra2_cuda.c`                | `ssimulacra2/{ssimulacra2_blur,ssimulacra2_mul}.cu`    | No default-model dep |

Total: 19 CUDA extractor entries (18 unique kernels, since `float_ssim_cuda`
piggybacks on `ssim_cuda.c`).

## Pre-round-2 coverage (after PR #351 merges)

Tests present in master + queued in PR #351:

- `test_cuda_motion3_parity` — `motion_cuda` (motion3 only, frame 1 EOS)
- `test_integer_vif_cpu_cuda_parity` — `vif_cuda` (4 scales + chroma no-op)
- `test_cuda_psnr_parity` — `psnr_cuda` (PR #351, round 1)
- `test_cuda_ciede_parity` — `ciede_cuda` (PR #351, round 1)

That is 4 of 18 kernels = ~22 % assertion coverage.

## Round-2 selection (this PR)

Picked the 5 highest-impact uncovered kernels, where impact = "appears
in the libvmaf-2.x.x shipped model lineage OR is a standard reference
companion to VMAF":

1. `adm_cuda` — default-model load-bearing.
2. `motion_v2_cuda` — `vmaf_v0.6.1neg` lineage; SAD reduction is the
   distinct kernel not exercised by `test_cuda_motion3_parity`.
3. `cambi_cuda` — `vmaf_v0.6.1neg` + `vmaf_4k_v0.6.1` lineage.
4. `psnr_hvs_cuda` — HVS-weighted DCT path; emitted in CHUG sidecars.
5. `integer_ssim_cuda` — standard SSIM companion; every CHUG sidecar
   carries it.

Post-round-2: 9 of 18 = 50 % assertion coverage. (Counting the dual
`adm_cuda` and `motion_v2_cuda` feature outputs separately would push
that toward ~55 %; for kernel-level book-keeping the 50 % figure is
the conservative one.)

## Omitted from round 2 (round-3 backlog)

Seven kernels remain uncovered after round 2; they cluster as the
"float variants + diagnostic helpers":

- `float_adm_cuda`, `float_motion_cuda`, `float_psnr_cuda`,
  `float_vif_cuda`, `float_ms_ssim_cuda` — none feeds the default model
  lineage. Useful for `--feature float_*` invocations but not blocking
  any score.
- `float_moment_cuda` — diagnostic feature, no model dep.
- `speed_chroma_cuda` + `speed_temporal_cuda` — performance-only
  helpers, no model dep.
- `ssimulacra2_cuda` — separate metric, used only with the
  `ssimulacra2` extractor explicitly. Round-3 candidate.

Round 3 should mechanically clone the round-2 scaffold for each of
these seven kernels (and ssimulacra2 with its own fixture geometry).

## PR overlap audit

Before drafting the round-2 tests, verified no overlap with in-flight PRs:

- **PR #351** (round 1) — adds `test_cuda_psnr_parity` and
  `test_cuda_ciede_parity`. Round 2 picks completely different kernels.
- **PR #289** (CUDA PTX module unload sweep) — modifies the kernel
  `.c` sources but adds **no** new test files. Round 2 only adds new
  test files; zero file-level overlap.

Verified via `gh pr view <id> --json files` on 2026-05-30.

## Tolerance rationale

ADR-0214 sets the cross-backend numerical gate at:

- `places=4` (1e-4) for unfiltered reductions (PSNR, CIEDE, ADM, motion).
- `places=3` (1e-3) for Gaussian-filtered kernels (VIF).

Round 2 uses `places=4` for all five tests, including SSIM. The 11x11
SSIM window on a 256x144 fixture stays well inside 1e-4 — the same
argument that justified `places=4` for PSNR / CIEDE in round 1. If a
follow-up CI run shows SSIM flapping, the per-file `PARITY_TOL` macro
is the single point of relaxation.

## Fixture geometry rationale

- 256x144 for adm / motion_v2 / psnr_hvs / ssim — same as round 1 and
  the existing `test_cuda_motion3_parity` / `test_integer_vif_cpu_cuda_parity`
  scaffolds. Comfortably above the ADM 4-scale DWT minimum and the 17-tap
  VIF / 11x11 SSIM stencils, but small enough to run in well under 1 s
  per test on the dev RTX 4090.
- 256x256 for cambi — CAMBI requires ≥ 216×216 on at least one
  dimension (`CAMBI_MIN_WIDTH_HEIGHT = 216` in both `cambi.c` and
  `integer_cambi_cuda.c`). 256x256 satisfies this on both axes while
  staying close to the other fixtures' compute cost.

## Verification

Container build (`vmaf-dev-mcp`, per CLAUDE.md §15) builds the new
test binaries. On a CUDA-equipped host, each `test_cuda_*_parity` is
expected to pass; on a CPU-only or non-CUDA host they emit `[skip:
no CUDA device]` and pass, matching the round-0 / round-1 skip
contract.

Smoke-test command for reviewers (when CUDA is present):

```text
meson setup build -Denable_cuda=true -Denable_sycl=false
ninja -C build
meson test -C build --suite=fast \
  test_cuda_adm_parity test_cuda_motion_v2_parity \
  test_cuda_cambi_parity test_cuda_psnr_hvs_parity \
  test_cuda_ssim_parity
```
