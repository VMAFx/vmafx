---
paths:
  - ai/scripts/train_fr_regressor_v2_ensemble*.py
  - ai/scripts/eval_probabilistic_proxy.py
  - ai/scripts/export_ensemble_v2_seeds.py
  - model/tiny/fr_regressor_v2_ensemble*
invariant: N=5 deep ensemble; registry flips smoke false only when all seeds pass PLCC >= 0.95 and spread <= 0.005.
---
<!-- markdownlint-disable MD013 MD060 -->
# `fr_regressor_v2_ensemble_v1` — probabilistic head (ADR-0393)

Probabilistic successor to codec-aware
`fr_regressor_v2` = deep ensemble of N=5 v2 members trained under
distinct seeds, packaged as 5 ONNX files plus manifest sidecar
`model/tiny/fr_regressor_v2_ensemble_v1.json`. Trainer:
[`ai/scripts/train_fr_regressor_v2_ensemble.py`](../scripts/train_fr_regressor_v2_ensemble.py);
evaluator: [`ai/scripts/eval_probabilistic_proxy.py`](../scripts/eval_probabilistic_proxy.py);
model card:
[`docs/ai/models/fr_regressor_v2_probabilistic.md`](../../docs/ai/models/fr_regressor_v2_probabilistic.md).

**Rebase-sensitive invariants:**

- **Strict helper annotations are part of CI boundary (ADR-1310).**
  `_synthesize_smoke_corpus()` returns exactly three `np.ndarray` values and
  `_load_ensemble_shapes()` uses parameterized manifest/session containers;
  keep those return contracts explicit when refactoring evaluator. Test
  callbacks added under `ai/tests/` also need typed variadic parameters, and
  parameterized tests must retain their signatures through typed marker
  adapter. required merge-base mypy gate treats new bare `dict`/`list`,
  untyped helper return, untyped decorator, or untyped injected callback as   branch finding.
- **Per-member ONNX I/O contract = v2 two-input shape**: inputs
  `features [N, 6]` (canonical-6, StandardScaler-normalised by
  manifest's `feature_mean` / `feature_std`) + `codec_onehot
  [N, NUM_CODECS]`; output `score [N]` float32. Each member = stock
  `FRRegressor(num_codecs=NUM_CODECS)` — flipping that to
  single-input v1-shaped graph silently invalidates every shipped
  ensemble.
- **Manifest layout = runtime entry point**, not registry
  rows. Each member also added to `model/tiny/registry.json` as
  `kind: "fr"` (with id `<ensemble_id>_seed<N>`), so existing
  tiny-model verifier can SHA-256-check each member without   schema bump. Manifest sidecar's `members[]` list = canonical
  ordered set C-side adapter iterates over. Adding schema-version
  field to `registry.schema.json` for `fr_ensemble` kind = future
  option; until then, ensemble lookups go through manifest, not
  registry.
- **Ensemble size is part of contract.** Manifest's
  `ensemble_size` field pins N; C-side adapter must open
  `ensemble_size` sessions. Changing N requires new ensemble id +
  manifest, not in-place mutation.
- **Confidence rule = one-of**: `confidence.method` either
  `"ensemble"` (use `gaussian_z` as multiplier on `sigma`) or
  `"ensemble+conformal"` (use `conformal_q_residual` instead).
  Trainer emits conformal scalar only when
  `--conformal-calibration-frac > 0` and calibration split is
  large enough; otherwise field stays `null`, Gaussian rule
  applies.
- **`CODEC_VOCAB` parity with v2 required.** Manifest pins
  `codec_vocab` + `codec_vocab_version`; runtime must refuse to
  load when these disagree with live `ai/src/vmaf_train/codec.py`
  vocabulary. Bumping vocabulary requires retraining ensemble;
  existing closed-vocabulary invariant from ADR-0235 carries over
  verbatim.
- **Historical smoke artefacts are retired.** ADR-0303 originally
  shipped synthetic 100-row / 1-epoch ensemble members as load-path
  probes. ADR-0321 replaced five seed ONNX files with
  full-corpus-trained production weights, added per-seed sidecars.
  Do not reintroduce `smoke: true` for
  `fr_regressor_v2_ensemble_v1_seed{0..4}` unless future ADR
  explicitly rolls production flip back.
- **Ensemble registry invariant (ADR-0303)**: each ensemble member's
  `smoke: true` registry row flips to `false` **only after** that
  individual seed clears `PLCC_i ≥ 0.95` LOSO ship gate
  (ADR-0235 / ADR-0291). Ensemble-mean entry — if/when one is
  added to `model/tiny/registry.json` as `fr_ensemble`-kind row —
  flips **only after all five seeds clear** per-seed gate *and*
  variance bound `max_i(PLCC_i) - min_i(PLCC_i) ≤ 0.005` holds.
  Decision lives in [`scripts/ci/ensemble_prod_gate.py`](../../scripts/ci/ensemble_prod_gate.py);
  trainer emitting per-seed `loso_seed{N}.json` artefacts gate
  consumes =
  [`ai/scripts/train_fr_regressor_v2_ensemble_loso.py`](../scripts/train_fr_regressor_v2_ensemble_loso.py).
  Do NOT flip individual seed rows by hand without running gate
  against real-corpus LOSO output. Variance bound protects
  predictive-distribution semantics; flipping seeds ad-hoc would
  silently bake in unbounded across-seed spread.
- **Registry-flip happened in ADR-0320**: five
  `fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
  `model/tiny/registry.json` flipped `smoke: true → false` on
  2026-05-06 against passing
  `runs/ensemble_v2_real/PROMOTE.json` (mean PLCC = 0.9973,
  spread = 9.5e-4, both gate components green) produced by
  [`ai/scripts/validate_ensemble_seeds.py`](../scripts/validate_ensemble_seeds.py).
  Verdict file committed at
  [`model/tiny/fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json`](../../model/tiny/fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json)
  as immutable audit trail. **Going-forward invariant**: any
  future registry change for these `ensemble_v1` seed rows (sha256
  bump after retraining, smoke-flag mutation, ONNX path change)
  requires fresh `PROMOTE.json` verdict with mean PLCC ≥ 0.95 AND
  spread ≤ 0.005. Same two-part gate ADR-0303 defined, ADR-0320
  honoured. **Never** flip or mutate these rows during   `/sync-upstream` rebase or as side-effect of any other PR —   harness in
  [`ai/scripts/run_ensemble_v2_real_corpus_loso.sh`](../scripts/run_ensemble_v2_real_corpus_loso.sh)
  and validator emit verdict file but **do not** mutate
  registry. Auto-flipping on PROMOTE was rejected in ADR-0309's
  alternatives matrix specifically because rebase-time mutation of
  shipped registry rows = foot-gun this invariant exists to
  prevent.
- **Ensemble production-flip now done (ADR-0321)**: as of
  2026-05-06 five `fr_regressor_v2_ensemble_v1_seed{0..4}` rows
  carry `smoke: false` and point at LOSO-gated, full-corpus-trained
  ONNX weights produced by
  [`ai/scripts/export_ensemble_v2_seeds.py`](../scripts/export_ensemble_v2_seeds.py).
  Each row has sidecar
  `model/tiny/fr_regressor_v2_ensemble_v1_seed{N}.json` that mirrors
  canonical `fr_regressor_v2.json` shape (encoder vocab v2, codec
  block layout, scaler params) plus seed-specific gate evidence from
  `runs/ensemble_v2_real/PROMOTE.json`. **Going-forward rule**: any
  future flip (re-train + re-export) requires fresh
  `PROMOTE.json` from LOSO trainer and re-run of
  `export_ensemble_v2_seeds.py`. Both ONNX bytes and
  per-seed sidecars must regenerate together so
  `test_registry.sh` sha256 + sidecar-presence check stays green.
  Editing one without other is foot-gun: registry test
  catches sha256 drift, but stale sidecar's gate-evidence block
  would silently lie about provenance.
- **Canonical-6 JSONL schema is load-bearing (ADR-0319)**: LOSO
  trainer's `_load_corpus` accepts schema emitted by
  [`scripts/dev/hw_encoder_corpus.py`](../../scripts/dev/hw_encoder_corpus.py)
  bit-for-bit — required keys per row =
  `(src, encoder, cq, frame_index, vmaf, adm2, vif_scale0..3, motion2)`.
  Codec block materialised as 12-slot `ENCODER_VOCAB` v2
  one-hot (mirrors `train_fr_regressor_v2.py`) + constant
  `preset_norm = 0.5` (corpus doesn't record preset) +
  `crf_norm` = `(cq - cq_min) / (cq_max - cq_min)`. Schema changes
  — column rename, encoder-vocab reorder, new required field —
  require `ENCODER_VOCAB_VERSION` bump and full ensemble retrain
  per existing closed-vocabulary invariant. Fold-level
  StandardScaler fit on training rows only (mirrors
  `eval_loso_vmaf_tiny_v3.py`); leaking held-out source's
  distribution into scaler would silently inflate per-fold
  PLCC. See ADR-0319 §Decision and `_load_corpus`'s docstring.
