---
paths:
  - core/src/feature/y_funque_plus.c
invariant: Y-FUNQUE+ atoms-only feature calculation and decomposition invariants.
area: ssim-and-metrics
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Y-FUNQUE+ Atoms-Only Feature Calculation

- **`y_funque_plus` atoms-only invariants** (ADR-1114): Y-FUNQUE+
  extractor (`y_funque_plus.c`) has **no upstream twin** and is
  clean-room reimplementation from papers (MIT `funque_plus` used
  only as cross-check). It ships **three atoms only**
  (`y_funque_plus_ms_ssim` / `_dlm` / `_mad`); fused ScaledSVR MOS
  score is deliberately **not** shipped (upstream commits no frozen
  regressor — see Deferred row in `docs/state.md`). Load-bearing
  details rebase or "cleanup" must not silently break:
  1. **Haar butterfly** uses pywt `'haar'` convention
     `cH=(a+b-c-d)/2`, `cV=(a-b+c-d)/2` (verified directly against
     `pywt.dwt2`). design dossier prose listed H/V **swapped** — do
     not "fix" code to match stale prose; code is correct
     and DLM's psi-angle mask depends on it.
  2. **DLM num/den abs-asymmetry** (`yf_dlm_pool`): numerator pools
     `rest^3` **without** abs while denominator pools ref detail
     **with** abs (mirrors upstream `pyr_features.py:54/61`). Symmetrising
     them is real behaviour change, not cleanup.
  3. **OpenCV `INTER_CUBIC`** (Keys cubic `a=-0.75`, src coord `2i+0.5`,
     `BORDER_REPLICATE`) is dominant cross-host parity component;
     TU compiles in its own static lib with `-ffp-contract=off` (
     same rationale as `ssimulacra2`). Keep both.
  4. Nadenau Y-channel CSF weights **only** detail subbands;
     approx subbands are never weighted. analytic constants
     (`a=1/256`, `b_Y=-5.4715e-3`, `c_Y=1.91`) regenerate official
     lookup table to 8 dp. Oracle values in `core/test/test_y_funque_plus.c`
     were re-derived against `pywt` + OpenCV reference at places=4.
