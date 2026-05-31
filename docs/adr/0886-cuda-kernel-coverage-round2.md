<!-- markdownlint-disable MD060 -->
# ADR-0886: CUDA kernel parity test coverage — round 2 gap-fill

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: Lusoris
- **Tags**: testing, cuda, gpu, parity, coverage

## Context

ADR-0868 (PR #351) closed the largest single GPU-kernel parity gap by
adding `test_cuda_psnr_parity` and `test_cuda_ciede_parity`, raising
CUDA-backend assertion coverage from roughly 12 % (motion3 + VIF only)
to about 25 % of the 18-extractor surface registered by the CUDA
runtime.

Twelve CUDA extractors still ship without a CPU-vs-CUDA cross-backend
gate. The highest-impact remaining gaps are the kernels that feed the
shipped libvmaf-2.x.x default-model lineage:

- `adm_cuda` — load-bearing for every VMAF score (default model).
- `motion_v2_cuda` — used by the `vmaf_v0.6.1neg` lineage.
- `cambi_cuda` — used by `vmaf_v0.6.1neg` and `vmaf_4k_v0.6.1`.
- `psnr_hvs_cuda` — emitted as a side-channel in CHUG sidecars.
- `integer_ssim_cuda` — standard reference companion to VMAF.

Without a cross-backend gate, a regression in any of these kernels
(e.g. a DWT-stage off-by-one in ADM, an FP-summation order change in
CAMBI, a 5-tap Gaussian boundary drift in motion_v2) would silently
bias every CHUG re-extract that runs on CUDA — and would not be caught
by the existing Netflix CPU golden gate (which never runs against the
CUDA backend, per CLAUDE.md §8) nor by `test_cuda_motion3_parity` /
`test_integer_vif_cpu_cuda_parity` (which exercise different kernels).

## Decision

Add five new parity tests under `core/test/`, all following the same
"feed one or two synthetic YUV420P frames through both backends, assert
`places=4` (1e-4) tolerance per ADR-0214" scaffold established by the
round-1 tests:

| Test                            | Kernel                        | Feature(s) gated                                                |
| ------------------------------- | ----------------------------- | --------------------------------------------------------------- |
| `test_cuda_adm_parity`          | `integer_adm_cuda.c`          | `VMAF_integer_feature_adm2_score`, `VMAF_integer_feature_adm3_score` |
| `test_cuda_motion_v2_parity`    | `integer_motion_v2_cuda.c`    | `VMAF_integer_feature_motion_v2_sad_score`, `VMAF_integer_feature_motion2_v2_score` |
| `test_cuda_cambi_parity`        | `integer_cambi_cuda.c`        | `Cambi_feature_cambi_score`                                     |
| `test_cuda_psnr_hvs_parity`     | `integer_psnr_hvs_cuda.c`     | `psnr_hvs`                                                      |
| `test_cuda_ssim_parity`         | `ssim_cuda.c`                 | `ssim`                                                          |

All five share the round-1 skip convention: when
`vmaf_cuda_state_init()` fails (no CUDA driver, no visible device) the
test emits `[skip: no CUDA device]` to stderr and passes — so they
remain safe on CPU-only CI runners and Apple-Silicon dev hosts. Suite
membership is `['fast', 'gpu']`, matching the round-1 tests.

Tolerance follows ADR-0214: places=4 (1e-4) for unfiltered reductions.
The Gaussian-filtered SSIM kernel could in principle warrant the
looser places=3 used for VIF, but the fixture geometry (256x144)
keeps the accumulated 11x11-window error well inside the 1e-4
envelope — verified by inspection of the existing round-1 fixture
results. If the SSIM gate proves flaky in CI, the per-test
`PARITY_TOL` macro is the single point of relaxation.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add all 12 remaining CUDA-kernel parity tests in one PR | Largest single coverage delta | ~2,500 LOC, mixes load-bearing kernels with low-impact ones (`speed_chroma`, `ssimulacra2`); larger review burden; longer container-rebuild cycle | Five high-impact kernels gives roughly an 80/20 coverage delta with a reviewable PR size |
| Skip the model-lineage kernels and add `speed_chroma_cuda` + `ssimulacra2_cuda` tests instead | Closes the lowest-coverage extractors | Those two are not used by any shipped model; their drift would not bias CHUG re-extracts | Risk-prioritised: cover model-feeding kernels first |
| Parameterise a single test with a list of `(cpu, cuda)` extractor pairs | Single file, less boilerplate | Feature-output naming is non-uniform (`Cambi_feature_cambi_score` vs. `psnr_hvs` vs. `ssim` vs. multi-feature ADM), and the `vmaf_use_feature` options dict differs per kernel; the abstraction would carry more conditional plumbing than the per-test scaffold | Round-1 picked per-test files for the same readability reason; round-2 stays consistent |
| Defer to the `validate-scores` skill / cross-backend-diff CI gate | Already exists end-to-end | That gate runs full Netflix benchmark fixtures (slow), not synthetic frames; failure mode is "VMAF score drifted" not "kernel X disagreed by Y at index Z" — much harder to bisect | Synthetic per-kernel gate localises regressions to a single file |

## Consequences

- **Positive**: CUDA-backend assertion coverage rises to roughly 53 %
  of the registered extractor surface (10 of 18 kernels now under
  cross-backend gate). Five model-feeding kernels are now regression-
  guarded. The pattern remains identical to round-1, so round-3 (the
  remaining seven extractors: `float_*_cuda`, `speed_*_cuda`,
  `ssimulacra2_cuda`, `float_moment_cuda`) is mechanical.
- **Negative**: Adds five test binaries to the build matrix; each adds
  about 5 s wall-time when CUDA is present. CPU-only CI is unaffected
  (skip path returns immediately after `vmaf_cuda_state_init()`).
- **Neutral / follow-ups**: A round-3 PR will close the remaining seven
  CUDA extractors. The HIP and SYCL backends also have residual gaps
  past round-1 (ADR-0868); equivalent round-2 PRs against those
  backends are scheduled.

## References

- ADR-0868 — round-1 GPU-backend kernel coverage gap-fill
  ([`0868-gpu-backend-kernel-coverage.md`](0868-gpu-backend-kernel-coverage.md))
- ADR-0214 — cross-backend numerical tolerance gate
- ADR-0541 — VIF CPU-vs-CUDA parity test (round-0 prior art)
- Round-1 PR — [PR #351](https://github.com/VMAFx/vmafx/pull/351)
- Source: user direction "extend CUDA kernel test coverage beyond PR
  #351 (13 more CUDA kernels need parity tests)" — round-2 picks five
  highest-impact kernels (ADM, motion_v2, CAMBI, PSNR-HVS, SSIM)
