<!-- markdownlint-disable MD013 MD060 -->
# ADR-0956: CUDA kernel parity coverage — round 4 (last 5 uncovered kernels)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude
- **Tags**: testing, cuda, parity, fork-local, gpu-coverage

## Context

The 2026-05-30 GPU-backend kernel coverage audit
(`docs/research/gpu-backend-kernel-coverage-audit-2026-05-30.md`, ADR-0868)
catalogued 19 CUDA feature-extractor registrations under
`core/src/feature/cuda/`. Round 1 (PR #351, ADR-0868) added 2 CUDA parity
tests (`psnr_cuda`, `ciede_cuda`); round 2 (PR #374, ADR-0886) queued 5
more (`adm_cuda`, `motion_v2_cuda`, `cambi_cuda`, `psnr_hvs_cuda`,
`integer_ssim_cuda`); round 3 (PR #442, ADR-0947) added 5 float-path
twins (`float_psnr_cuda`, `float_vif_cuda`, `float_ms_ssim_cuda`,
`float_moment_cuda`, `ssimulacra2_cuda`). Combined with the
pre-existing master gates (`test_cuda_motion3_parity.c`,
`test_integer_vif_cpu_cuda_parity.c`) that brings cumulative coverage
to 14 of 19 kernels (~74 %).

This ADR closes the last 5 uncovered kernels:

| Kernel | CPU twin | Gate type |
|---|---|---|
| `float_adm_cuda` | `float_adm` | CPU-vs-CUDA parity (ADR-0214) |
| `float_motion_cuda` | `float_motion` | CPU-vs-CUDA parity (ADR-0214) |
| `float_ssim_cuda` | `float_ssim` | CPU-vs-CUDA parity (ADR-0214) |
| `speed_chroma_cuda` | (none — CUDA-only) | finite-score smoke |
| `speed_temporal_cuda` | (none — CUDA-only) | finite-score smoke |

The first three follow the round-3 parity-test template verbatim. The
two `speed_*_cuda` extractors are **CUDA-only** — there is no CPU
twin emitting the same `Speed_{chroma,temporal}_feature_*_score`
family (the closest CPU surface, `speed_qa`, computes a different
spatial+temporal scalar). For those, a CPU-vs-CUDA parity assertion
is the wrong tool; instead the gate is a smoke test that runs the
extractor end-to-end on a CUDA device and asserts finite, sane
output. That catches NaN/Inf drift from kernel-grid changes or
covariance-matrix degenerate cases — the failure modes the ADR-0567
chroma/temporal eigendecomp path is exposed to.

## Decision

Add **five fork-local tests** under `core/test/`:

| Test file | Kernel | Probed feature(s) |
|---|---|---|
| `test_cuda_float_adm_parity.c` | `float_adm_cuda` | `VMAF_feature_adm2_score` + four `adm_scale[0..3]_score` |
| `test_cuda_float_motion_parity.c` | `float_motion_cuda` | `VMAF_feature_motion_score`, `..._motion2_score` (motion3 is host-side; covered by `test_cuda_motion3_parity.c`) |
| `test_cuda_float_ssim_parity.c` | `float_ssim_cuda` | `float_ssim` |
| `test_cuda_speed_chroma_smoke.c` | `speed_chroma_cuda` | three `Speed_chroma_feature_speed_chroma_{u,v,uv}_score` |
| `test_cuda_speed_temporal_smoke.c` | `speed_temporal_cuda` | `Speed_temporal_feature_speed_temporal_score` |

The three parity tests follow the round-3 `test_cuda_float_psnr_parity.c`
template: 256x144 YUV420P 8-bpc synthetic fixture, 3 frames, score
read at frame index 1, `places=4` / 1e-4 tolerance (ADR-0214),
`[skip: no CUDA device]` guard when `vmaf_cuda_state_init` fails.

The two smoke tests use a larger 640x360 fixture so the operating
resolution after the ADR-0567 2^4 decimation (40x22 luma, 20x11
chroma) admits a non-singular covariance matrix; they assert finite
scores and skip cleanly when no CUDA device is visible.

All five wire into `core/test/meson.build` behind the existing
`get_option('enable_cuda')` guard, register with suite
`['fast', 'gpu']`, and link the same `(pthread, cuda, math)`
dependency triple as `test_cuda_motion3_parity`.

Post-PR CUDA-extractor coverage rises from **14 of 19** (~74 %, after
PRs #351 / #374 / #442 land) to **19 of 19** (100 %) once this PR
merges. The kernel-coverage backlog is then closed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add parity tests only (3 float twins) and defer the speed kernels again | Simpler review, fewer test files | The `speed_*_cuda` kernels are exactly the ones most likely to silently drift (host-side eigendecomp with degenerate-matrix paths); deferring again kicks the can a fourth round | Smoke gates close the gap without inventing CPU twins; the cost is two ~150 LOC files |
| Synthesise a CPU reference for `speed_*` (port the eigendecomp to a CPU-only TU and parity-test against it) | Tightest possible gate | The CPU reference is exactly what the CUDA kernel calls (`speed_internal_*` is the host-side path); we'd be parity-testing against ourselves | The host-side eigendecomp is shared code; the smoke test still catches kernel drift without a redundant reference |
| Use 1920x1080 fixture (matches python/test `set_default_speed_chroma_edge_case`) | Maximum coverage of operating resolution | ~50× slowdown for no extra fault detection | 640x360 already exercises 32 blocks (temporal) and 8 blocks (chroma) — enough for non-degenerate eigendecomp |
| Skip ADR-0956 and let drift surface via CHUG re-extracts | Zero new test surface | CHUG re-extracts take hours and only surface drift after the bad commit has shipped; smoke tests fire in seconds in `--suite=fast` | Defeats the point of the coverage push (ADR-0868 thesis: early detection beats late) |

## Consequences

- **Positive**: closes the kernel coverage backlog (19 of 19 CUDA
  feature extractors gated); pins the last float-path numerical
  contract; smoke gates on the host-side eigendecomp paths catch the
  NaN/Inf failure modes the ADR-0567 chroma/temporal path is exposed
  to.
- **Negative**: +5 test binaries (~1100 LOC) to the build matrix; ~3 s
  added to `meson test --suite=gpu` wall time on a CUDA-enabled runner.
- **Neutral / follow-ups**: HIP and SYCL kernel coverage are tracked
  separately (ADR-0945 in flight; SYCL backlog open). The CUDA path
  is now feature-complete.

## References

- [ADR-0214](0214-cross-backend-parity-gate.md) — cross-backend tolerance gate (`places=4`)
- [ADR-0567](0567-speed-chroma-temporal-real-gpu.md) — speed_chroma/temporal CUDA real-impl + host eigendecomp
- [ADR-0868](0868-gpu-backend-kernel-coverage.md) — round 1 (psnr + ciede)
- [ADR-0886](0886-cuda-kernel-coverage-round2.md) — round 2 (adm/motion_v2/cambi/psnr_hvs/ssim)
- [ADR-0947](0947-cuda-kernel-coverage-round3.md) — round 3 (float-path twins + ssimulacra2)
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six-deliverables rule
- `docs/research/gpu-backend-kernel-coverage-audit-2026-05-30.md` — original audit + backlog
- Source: req (CUDA kernel coverage round 4 — close the last 5 uncovered CUDA kernels)
