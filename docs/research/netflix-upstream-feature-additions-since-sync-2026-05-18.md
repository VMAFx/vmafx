<!-- markdownlint-disable MD060 -->
# Research Digest: Netflix Upstream Feature Additions — CUDA Twin Gap Audit (2026-05-18)

**Task**: Identify which C-side sub-features consumed by the Netflix HDR VMAF
model are present in the fork's CPU code but lack a CUDA twin, and determine the
correct porting strategy for each.

**Scope**: Five candidate features reported by the user — `aim`, `adm3`,
`motion3`, `chroma_from_luma`, `cambi_eotf`/`effective_eotf`.

---

## Findings

### 1. `aim` — Anchored Integer Motion (adm sub-feature)

- **CPU location**: `core/src/feature/float_adm.c` and
  `core/src/feature/adm.c`.
- **Mechanism**: The `compute_adm()` function in `adm.c` runs a second
  `adm_cm` pass per scale with the CSF of `decouple_r` (not `decouple_a`)
  and with `noise_weight = 0`. The result is accumulated into
  `aim_num` / `aim_den` and finally emitted as
  `VMAF_feature_aim_score` from `float_adm.c`'s `extract()`.
- **CUDA status before this PR**: Missing. `float_adm_cuda.c` did not
  emit `VMAF_feature_aim_score`. `--backend cuda` silently fell back to
  CPU when the HDR model requested this feature.
- **Fix**: Two new kernel stages (2b and 3b) added to
  `float_adm_score.cu`. See ADR-0574.

### 2. `adm3` — ADM Version 3 (adm sub-feature)

- **CPU location**: `core/src/feature/float_adm.c`.
- **Mechanism**: Derived in `extract()` from `score` (adm2) and
  `score_aim` (AIM) as either a harmonic mean (when
  `adm_adm3_apply_hm=true`) or a linear blend weighted by
  `adm_dlm_weight`. Emitted as `VMAF_feature_adm3_score`.
- **CUDA status before this PR**: Missing (same root cause as `aim` —
  no AIM accumulation in `float_adm_cuda.c`).
- **Fix**: Same two new kernel stages; host-side `collect_fex_cuda`
  derives and emits `adm3_score` from the AIM accumulation. ADR-0574.

### 3. `motion3` — Motion Version 3 (motion sub-feature)

- **CPU location**: `core/src/feature/integer_motion.c`.
- **Mechanism**: Post-processing of motion2 via a moving average of
  blended scores; options `motion_blend_factor`, `motion_blend_offset`,
  `motion_fps_weight`, `motion_moving_average`, `motion_max_val`.
  Emitted as `VMAF_integer_feature_motion3_score`.
- **CUDA status**: Already implemented in `integer_motion_cuda.c`
  (`motion3_postprocess_cuda()`, ADR-0219). No porting needed.

### 4. `chroma_from_luma` — Chroma Prediction Correction

- **Investigation**: Searched for `chroma_from_luma` across
  `core/src/feature/` and `core/include/`. Found references only
  in `core/src/model.h` and `core/src/model.c` as a field of the
  model's predictor configuration, not as a feature extractor.
- **CUDA status**: Not applicable — this is a model-level predictor
  attribute, not a GPU kernel. No feature extractor symbol
  `vmaf_fex_*_chroma_from_luma` exists. No porting needed.

### 5. `cambi_eotf` / `effective_eotf` — PQ/HLG EOTF Support in CAMBI

- **CPU location**: `core/src/feature/cambi.c`.
- **Mechanism**: `eotf` option selects PQ/HLG inverse EOTF pre-processing
  applied to the input luma plane before CAMBI computation.
  `effective_eotf` is the resolved EOTF name stored in feature output.
- **CUDA status**: Already implemented in `integer_cambi_cuda.c` with the
  same `eotf` and `cambi_eotf` options (`HAVE_CUDA` block). No porting
  needed.

---

## Summary Table

| Feature | CPU location | CUDA status | Action |
|---|---|---|---|
| `aim` | `float_adm.c` / `adm.c` | **Missing** | Ported — ADR-0574 |
| `adm3` | `float_adm.c` | **Missing** | Ported — ADR-0574 |
| `motion3` | `integer_motion.c` | Already done (ADR-0219) | None |
| `chroma_from_luma` | `model.h` (predictor field) | N/A — not an extractor | None |
| `cambi_eotf` | `cambi.c` | Already done | None |

---

## Implementation approach chosen for aim / adm3

The AIM CM computation reuses the same DWT band buffers produced by
stages 0 and 1. The key difference from the adm2 CM pass (stage 3) is:

- The CSF threshold is derived from `decouple_r = k * o` (the
  remodulated reference component), not from `decouple_a = t - r`
  (the anomaly).
- `noise_weight = 0` — no noise constant is added to the CM numerator.

Rather than storing `decouple_r` in a scratch buffer (which would cost
an extra device-side buffer the same size as `csf_a`), the two new
kernels recompute `k` and `r_val` inline from the already-buffered
`ref_band` / `dis_band` arrays. This trades a few arithmetic ops per
pixel for zero extra bandwidth, which is favourable at all tested
resolutions.

The accumulator slot layout extension (6→9) adds 50 % to the pinned
D2H copy per scale but the absolute size is a few KB — negligible
compared to the kernel execution time.
