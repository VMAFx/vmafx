<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1203: `psnr_hvs_cuda` defaults `enable_chroma` to true, matching every other backend

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cuda, correctness, feature-extractor, options

## Context

`test_cuda_psnr_hvs_parity` has been failing in the `fast` suite with
`cpu=9.99749385 cuda=9.60927651`, a delta of 3.9e-01 against a 1e-4 tolerance —
roughly 4000x over. It was being carried as a known-failing test.

It is not a tolerance problem. The two backends were computing different
quantities under the same feature name.

`psnr_hvs` is defined upstream as the YCbCr-weighted score
`0.8*Y + 0.1*(Cb + Cr)`. The fork added an `enable_chroma` option as an
opt-*out* for callers who want the cheaper luma-only value. The CPU twin
(`third_party/xiph/psnr_hvs.c`) documents this explicitly and defaults it to
`true` so that a caller who sets nothing gets the upstream-equivalent result.
The SYCL twin also defaults to `true`, and the HIP twin computes
`0.8*Y + 0.1*(Cb + Cr)` unconditionally with no option at all.

The CUDA twin defaulted it to `false`. So `--feature psnr_hvs_cuda` silently
returned the luma-only score under the name `psnr_hvs`, and omitted
`psnr_hvs_cb` and `psnr_hvs_cr` from its output entirely, even though its
`provided_features[]` advertises all four and
[`docs/metrics/features.md`](../metrics/features.md) lists CUDA as providing
them.

Measured on a 960x540 pair: CPU `psnr_hvs` 41.7803055708, CUDA `psnr_hvs`
41.4866616015 — and CUDA's value equals the CPU twin's *luma-only*
`psnr_hvs_y` (41.4870099914) to within 3.5e-04, which is what identified the
cause.

This also reaches the tiny-AI training set: `psnr_hvs` is a member of
`FULL_FEATURES` in `ai/data/feature_extractor.py`, and
`ai/scripts/extract_k150k_features.py` computes it via `psnr_hvs_cuda` on the
CUDA pass. Training rows therefore carried the luma-only value while a CPU-mode
extraction of the same clip would carry the chroma-weighted one.

## Decision

`psnr_hvs_cuda` will default `enable_chroma` to `true`, matching the CPU and
SYCL twins and HIP's unconditional behaviour. The option remains available as an
explicit opt-out.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Default CUDA `enable_chroma` to `true` (chosen) | One-line change; makes all four backends agree; `test_cuda_psnr_hvs_parity` passes; restores the documented output set | CUDA `psnr_hvs` values change, and chroma planes cost GPU time | — |
| Relax the parity test's tolerance | Test goes green immediately | The backends would still compute different quantities under one name. This is test-weakening over a real defect | Rejected outright: the tolerance was correctly reporting a genuine 4% semantic divergence |
| Default the CPU twin to `false` instead | Also makes the twins agree | Breaks upstream equivalence and the `third_party/xiph` PSNRHVS golden assertions, and silently drops chroma for every existing CPU caller | Aligns the wrong way — CUDA was the sole outlier among four backends |
| Move `psnr_hvs` to the CPU residual pass in the extraction script | Fixes the training data without touching C | Leaves the CLI defect in place for every other `psnr_hvs_cuda` user, and gives up the GPU speedup | Treats a symptom in one consumer instead of the defect |
| Document CUDA as luma-only | No behaviour change | Two backends disagreeing under one feature name is a bug, not a documentable variation | Cross-backend agreement is the fork's contract (ADR-0214) |

## Consequences

- **Positive**: `test_cuda_psnr_hvs_parity` passes. CPU↔CUDA `psnr_hvs` agreement
  improves from 2.9e-01 to 3.0e-04 on the 960x540 measurement pair.
  `psnr_hvs_cb` / `psnr_hvs_cr` are now emitted on CUDA as
  `provided_features[]` and the docs already claimed.
- **Negative**: `psnr_hvs_cuda` scores change for any caller that relied on the
  old default, and the CUDA extractor now dispatches three planes instead of
  one, so it costs more GPU time. Both are the price of computing the metric
  the name denotes.
- **Neutral / follow-ups**: the residual CPU↔CUDA delta on `psnr_hvs_y`
  (3.5e-04) is ordinary float accumulation order and stays within the parity
  test's per-plane behaviour. `test_cuda_float_adm_parity` remains failing for
  an unrelated numerical reason — its option defaults were checked and do match.

## References

- `core/src/feature/cuda/integer_psnr_hvs_cuda.c`,
  `core/src/feature/third_party/xiph/psnr_hvs.c`,
  `core/src/feature/sycl/integer_psnr_hvs_sycl.cpp`,
  `core/src/feature/hip/integer_psnr_hvs_hip.c`
- [ADR-0214](0214-cross-backend-tolerance.md) — the places=4 cross-backend gate
  this test enforces
- `ai/data/feature_extractor.py` (`FULL_FEATURES`),
  `ai/scripts/extract_k150k_features.py` (`CUDA_EXTRACTOR_NAMES`)
