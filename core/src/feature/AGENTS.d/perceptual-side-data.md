---
paths:
  - core/src/feature/feature_collector.cpp
  - core/src/feature/feature_extractor.h
  - core/src/feature/pelorus_udu_wrap.h
  - core/src/feature/perceptual_weight.c
invariant: Perceptual side-data weighting golden-isolation invariant and normalization contracts.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Perceptual Side-Data Weighting Golden-Isolation

- **Perceptual side-data weighting golden-isolation invariant**
  (`perceptual_weight.{c,h}` + weighting branch in
  `core/src/libvmaf.c::vmaf_feature_score_pooled`, fork-local, ADR-1118):
  Pelorus-driven pooling weights MUST be **inert** unless BOTH () weighting is
  enabled (`vmaf_set_perceptual_weight_enabled`, default OFF) AND (b) valid
  Pelorus blob was registered for frame (`vmaf_set_perceptual_sidedata`).
  This is **load-bearing for Netflix golden gate** — golden pairs carry
  no side-data, so they must score **bit-exact**. Two rules survive any refactor:
  1. `perceptual_weight.c::vmaf_perceptual_weight_at_index` returns **exactly
     `1.0`** for any disabled / empty / absent / non-finite case. Never let it
     return "close to 1.0" value on no-side-data path.
  2. `vmaf_feature_score_pooled` must branch on
     `vmaf_perceptual_weight_active()`: when inactive it runs **literal
     upstream** MEAN / HARMONIC_MEAN expressions (`sum/pic_cnt`,
     `pic_cnt/i_sum − 1`) in original order — NOT weighted formula that
     merely evaluates to same number. Byte-identical, not numerically close.
     weighted accumulators (`w_sum` / `w_score_sum` / `w_i_sum`) are only
     summed when active. MIN/MAX are intentionally never weighted.
  end-to-end guard is `core/test/test_perceptual_weight.c` (bit-exact
  pooling without side-data, with enabled-but-absent, and with present-but-
  disabled). reader is CPU-only and consumes vendored Pelorus parser
  (ADR-1113) — do not edit those vendored files. R1–R6 graceful-degrade
  (`grid==0` → frame-level scalar; bad ABI → unweighted + log) must also hold.
  3. **Complexity modulation (ADR-1120, Pelorus ABI ≥ 1.3):**
     `derive_salience` scales salience by
     `complexity_modulation(blob)` = `(1 − 0.5·complexity)` floored at `0.25`
     when `PEL_SEC_COMPLEXITY` is present. This MUST collapse to factor `1.0`
     (no modulation) when section is **absent** or `complexity` is
     non-finite — that is what keeps no-side-data golden path bit-exact.
     Never change `complexity_modulation` to return anything but `1.0` on
     absent/NaN path. load-bearing guard is `test_complexity_modulates_weight`
     / `test_complexity_modulates_grid_zero` (toggle-proven: stubbing helper
     to no-op fails test). section is grid-independent, so it also
     modulates `grid==0` scalar path.

## Pelorus side data behind the x265 UUID (ADR-3340)

- **Allow-list, one outer UUID.** `vmaf_pelorus_udu_classify()`: DIRECT =
  blob or carrier UUID at offset 0; X265_WRAPPED = x265 UUID
  `2ca2de09-b517-47db-bb55-a4fe7fc2fc4e` at 0, more than 16 data bytes, blob or
  carrier UUID at 16; NONE = rest. Never skip an unknown UUID and retry; never
  accept a second x265 wrap (Pelorus ADR-0210).
- **Fork code, not mirror.** Classifier lives in `core/src/feature/`; the
  Pelorus mirror (`pel_blob_unwrap()`, `libvmaf/pelorus/*.h`) stays
  byte-identical to the pin (ADR-1113).
- **Vectors = Pelorus' file.** `testdata/pelorus/udu-wrap-vectors.txt` copied
  from VMAFx/pelorus `ffmpeg-patches/test/fixtures/udu-wrap-vectors.txt`
  (blob `3a115ee3` at `7f6bd706`). Pelorus adds vectors: copy file, raise
  `VEC_MIN_COUNT` in `core/test/test_pelorus_udu_wrap.c`. Never edit a vector
  here.
- **Guards.** `test_pelorus_udu_wrap` (replay, fails on a missing file, too few
  vectors, a class without one); `test_perceptual_weight`
  `test_x265_wrapped_matches_direct` (bit-identical weight, blob and carrier),
  `test_unknown_outer_and_double_wrap_refused` (-ENOENT).
