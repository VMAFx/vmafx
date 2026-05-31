<!-- markdownlint-disable MD060 -->
# ADR-0574: CUDA Twins for HDR-Model Features — Phase 1 (aim, adm3)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `cuda`, `feature`, `hdr`, `adm`

## Context

The Netflix HDR VMAF model consumes five C-side sub-features that were
already present in the CPU code but had no CUDA twin: `aim`
(Anchored Integer Motion), `adm3`, `motion3`, `chroma_from_luma`, and
`cambi_eotf`/`effective_eotf`. Without CUDA twins, `--backend cuda`
silently fell back to CPU for these sub-features, defeating the purpose
of GPU selection.

An audit (documented in
`docs/research/netflix-upstream-feature-additions-since-sync-2026-05-18.md`)
found:

- `motion3` — already ported to CUDA in `integer_motion_cuda.c`
  (emits `VMAF_integer_feature_motion3_score`).
- `cambi_eotf` / `effective_eotf` — already ported in
  `integer_cambi_cuda.c`.
- `chroma_from_luma` — this is a predictor field in the model (model.h),
  not a feature extractor; no kernel is needed.
- `aim` and `adm3` — genuinely missing from `float_adm_cuda.c`.

This ADR covers the `aim` and `adm3` additions (Phase 1).

## Decision

We will extend `float_adm_cuda.c` and `float_adm/float_adm_score.cu`
to compute and emit `VMAF_feature_aim_score` and
`VMAF_feature_adm3_score` from the CUDA `float_adm` extractor, matching
the CPU `float_adm.c` implementation to `places=4`.

The implementation adds two new kernel stages per scale:

- **Stage 2b** (`float_adm_csf_r`): Computes CSF of `decouple_r`
  (the remodulated component), writing `csf_a_aim` and `csf_f_aim`.
  This mirrors the CPU `adm_csf(&decouple_r, ...)` call.
- **Stage 3b** (`float_adm_aim_cm`): Computes the AIM CM numerator
  using `decouple_a` masked by the `csf_a_aim`/`csf_f_aim` threshold,
  with `noise_weight = 0`. Results land in accumulator slots 6..8.

The `FADM_ACCUM_SLOTS` constant is extended from 6 to 9:
`[0..2]` = csf\_den, `[3..5]` = cm\_num, `[6..8]` = aim\_cm per band.

The host-side `collect()` reads slots 6..8 and computes:

- `score_aim = MIN(aim_num / aim_den, 1.0)`
- `score_adm3` via harmonic mean or linear blend depending on
  `adm_adm3_apply_hm` / `adm_dlm_weight`

Six new options are exposed with the same defaults as CPU `float_adm.c`:
`adm_bypass_cm`, `adm_adm3_apply_hm`, `adm_p_norm`, `adm_dlm_weight`,
`adm_min_val`, `adm_skip_aim_scale`.

The `--fmad=false` nvcc flag already applied to `float_adm_score.cu`
(required for `places=4` parity, per AGENTS.md) covers the new kernels.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Separate `.cu` file for AIM kernels | Clean file boundaries | Requires second PTX load, extra meson entry, duplicated DWT band reads | Cost exceeds benefit; kernels share helper functions |
| Store `decouple_r` in a scratch buffer between stages 2 and 3 | Avoids recomputation in stage 3b | Extra buffer allocation (same size as csf_a), extra write+read per pixel | Recomputation is cheap (few flops); bandwidth trade is unfavourable |
| Emit AIM only when explicitly requested via option flag | Saves 2 kernel launches when unused | Complicates dispatch logic; HDR model needs AIM unconditionally | Unconditional is simpler; 2 extra launches per scale are inexpensive |

## Consequences

- **Positive**: `--backend cuda` now produces `VMAF_feature_aim_score`
  and `VMAF_feature_adm3_score` without CPU fallback; HDR VMAF model
  runs fully on GPU.
- **Positive**: Stages 2b and 3b reuse DWT bands already resident in
  L2 from stages 2 and 3, keeping the marginal latency low.
- **Negative**: Per-frame launch count increases from 16 to up to 24
  (6 stages × 4 scales; stage 3b is skipped for `adm_skip_aim_scale`).
- **Negative**: `FADM_ACCUM_SLOTS = 9` increases the pinned D2H copy
  size by 50% (6 to 9 floats per WG). At 1080p the accum buffer is a
  few KB per scale — negligible.
- **Negative**: Two new device buffers (`csf_a_aim`, `csf_f_aim`) each
  sized at `FADM_NUM_BANDS * buf_stride * scale_half_h[0] * 4` bytes
  (same as `csf_a`/`csf_f`). At 1080p this is approximately 3.5 MB
  additional VRAM.
- **Neutral / follow-ups**: SYCL and Vulkan `float_adm` twins do not
  yet emit `aim_score` / `adm3_score`; those are Phase 2.

## References

- req: user task message 2026-05-18: "port CUDA twins for aim, adm3,
  motion3, chroma_from_luma, cambi_eotf"
- `docs/research/netflix-upstream-feature-additions-since-sync-2026-05-18.md`
- ADR-0192 / ADR-0202 — original float_adm CUDA twin specification
- AGENTS.md in `core/src/feature/cuda/` — `--fmad=false` invariant
  and `cuLaunchKernel` arg-pointer rule
- ADR-0535: ADR atomic allocator
