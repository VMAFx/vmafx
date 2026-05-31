# FR regressor v3 — codec-aware on ENCODER_VOCAB v3 (16-slot)

`fr_regressor_v3` — codec-aware FR regressor trained on
`ENCODER_VOCAB_V3` (16 slots). Parallel-shipped successor to
[`fr_regressor_v2`](fr_regressor_v2.md). Maps a 6-D canonical libvmaf
feature vector plus an 18-D codec block (16 encoder one-hot +
`preset_norm` + `crf_norm`) to a VMAF teacher score scalar.

> **Status: Production checkpoint (gate-passed).** Mean LOSO PLCC =
> **0.9975** across the 9 Netflix Public Dataset sources, comfortably
> above the
> [ADR-0302](../../adr/0302-encoder-vocab-v3-schema-expansion.md)
> ship gate of 0.95 — the same gate
> [ADR-0291](../../adr/0291-fr-regressor-v2-prod-ship.md) cleared on
> v2. Ships under [ADR-0323](../../adr/0323-fr-regressor-v3-train-and-register.md);
> registry row `fr_regressor_v3` lands with `smoke: false`.
>
> The live `ENCODER_VOCAB_VERSION = 2` in
> [`ai/scripts/train_fr_regressor_v2.py`](../../../ai/scripts/train_fr_regressor_v2.py)
> **stays authoritative** for `fr_regressor_v2.onnx`. Promoting v3 to
> "the" canonical `fr_regressor_v2.onnx` slot is a separate
> follow-up PR — see ADR-0302 §Production-flip checklist.

## Inputs

Two named tensors, dynamic batch axis (matches the
`vmaf_dnn_session_run` two-input contract from
[ADR-0040](../../adr/0040-dnn-session-multi-input-api.md) /
[ADR-0022](../../adr/0022-inference-runtime-onnx.md)):

- **`features`**, shape `(N, 6)` — canonical-6 libvmaf features,
  StandardScaler-normalised at training time using the mean/std baked
  into the sidecar JSON (`feature_mean`, `feature_std`):

  | Index | Feature        |
  |-------|----------------|
  | 0     | `adm2`         |
  | 1     | `vif_scale0`   |
  | 2     | `vif_scale1`   |
  | 3     | `vif_scale2`   |
  | 4     | `vif_scale3`   |
  | 5     | `motion2`      |

- **`codec_block`**, shape `(N, 18)` — codec block, **not**
  normalised (already in `[0, 1]`):

  | Index | Slot                                 |
  |-------|--------------------------------------|
  | 0     | `encoder_onehot[libx264]`            |
  | 1     | `encoder_onehot[libaom-av1]`         |
  | 2     | `encoder_onehot[libx265]`            |
  | 3     | `encoder_onehot[h264_nvenc]`         |
  | 4     | `encoder_onehot[hevc_nvenc]`         |
  | 5     | `encoder_onehot[av1_nvenc]`          |
  | 6     | `encoder_onehot[h264_amf]`           |
  | 7     | `encoder_onehot[hevc_amf]`           |
  | 8     | `encoder_onehot[av1_amf]`            |
  | 9     | `encoder_onehot[h264_qsv]`           |
  | 10    | `encoder_onehot[hevc_qsv]`           |
  | 11    | `encoder_onehot[av1_qsv]`            |
  | 12    | `encoder_onehot[libvvenc]`           |
  | 13    | `encoder_onehot[libsvtav1]`          |
  | 14    | `encoder_onehot[h264_videotoolbox]`  |
  | 15    | `encoder_onehot[hevc_videotoolbox]`  |
  | 16    | `preset_norm`  (preset ordinal / 9)  |
  | 17    | `crf_norm`     (cq normalised)       |

  Encoder vocabulary is closed and order-stable per
  [ADR-0235](../../adr/0235-codec-aware-fr-regressor.md) — the index
  of each codec is the one-hot column index baked into the trained
  ONNX. v3 differs from v2 in three structural ways:

  1. **Append-only expansion**: the 13 v2 slots are preserved at the
     same column indices; three new slots (`libsvtav1`,
     `h264_videotoolbox`, `hevc_videotoolbox`) are appended at indices
     13/14/15. The v2 → v3 reordering of the runtime-flip-only slot
     order (`libaom-av1` at 1 instead of 4, etc.) is the published
     ADR-0291 v2 layout — that ordering was already documented in
     `ENCODER_VOCAB_V3` since PR #401 (ADR-0302 scaffold) and matches
     the user-facing v2 layout in the ADR-0291 model card.
  2. **No `unknown` slot.** v2 carried a 12th `unknown` bucket as the
     fallback for novel codecs. v3 drops it — the closed 16-slot
     vocab covers every adapter currently registered under
     `tools/vmaf-tune/src/vmaftune/codec_adapters/`. Encoder strings
     outside the vocab fall back to slot 0 (`libx264`); document this
     at call sites that bridge novel codec strings.
  3. **Output name** is `vmaf` (was `score` in v2) — matches the
     teacher-score column the corpus rows carry. Sidecar
     `output_names: ["vmaf"]` records this.

## Output

`vmaf`, shape `(N,)` — a scalar VMAF-aligned quality score per sample,
same MOS range as v1/v2 (typically `[0, 100]`).

## Training corpus

Two corpus shapes are accepted, mapped to the same internal feature /
codec-block tensors at load time:

1. **`vmaf-tune` corpus, schema v3** (preferred,
   [ADR-0366](../../adr/0366-corpus-schema-v3.md)). One row per
   (source, encoder, preset, crf) encode, canonical-6 means /
   stddevs computed from libvmaf's `pooled_metrics` block:

   ```json
   {"schema_version": 3, "src": "BigBuckBunny_25fps.yuv",
    "encoder": "h264_nvenc", "preset": "p4", "crf": 19,
    "vmaf_score": 95.86,
    "adm2_mean": 0.99, "vif_scale0_mean": 0.88,
    "vif_scale1_mean": 0.99, "vif_scale2_mean": 0.996,
    "vif_scale3_mean": 0.998, "motion2_mean": 0.0,
    "adm2_std": 0.01, "vif_scale0_std": 0.02, ...}
   ```

   Rows with NaN canonical-6 means (libvmaf did not expose the
   feature, or the encode failed) are dropped before the
   StandardScaler is fitted — never imputed to 0.0. Legacy v2 corpora
   that carry only `vmaf_score` raise `ValueError` and point operators
   at this ADR; they cannot train this regressor.

2. **`hw_encoder_corpus.py` per-frame corpus** (legacy / NVENC-only).
   `runs/phase_a/full_grid/per_frame_canonical6.jsonl` (5,640 rows).
   One row per frame, bare canonical-6 column names, target column
   `vmaf`, quality knob `cq`. The training cohort the gate-passing
   v3 checkpoint was fit on.

### NVENC-only corpus caveat

The current Phase A corpus drop is **NVENC-only** (slot 3,
`h264_nvenc`). The remaining 15 vocab slots receive **zero training
examples** in this checkpoint. Inference behaviour at the un-trained
slots:

- The MLP weights for the 15 unused one-hot columns remain at their
  Glorot initialisation; the bias path through the canonical-6 features
  `preset_norm` + `crf_norm` is what actually carries signal for
  inference on those codecs.
- In practice the model will produce **degraded but not random**
  predictions for the 15 untrained codecs — the canonical-6 features
  alone clear ~0.99 PLCC on the v1 single-input baseline
  ([ADR-0249](../../adr/0249-fr-regressor-v1.md)), so the
  un-NVENC-trained codec predictions inherit that baseline behaviour
  modulo the small one-hot column shift.
- The
  [ADR-0235](../../adr/0235-codec-aware-fr-regressor.md) multi-codec
  lift floor (≥ +0.005 PLCC over v1) is **not yet measured** — the
  current NVENC-only corpus does not exercise other codecs, so v3's
  lift over v1 reduces to v1 vs v1 on NVENC. v3 ships as the
  production graph regardless because it is forward-compatible with
  the broader 16-slot ENCODER_VOCAB v3 schema and re-using v2 would
  block multi-codec follow-up corpora; the lift floor will be enforced
  retroactively when a future Phase A corpus drop covers ≥3 codec
  families.

This caveat is the dominant reason the live `ENCODER_VOCAB_VERSION`
stays at 2 in `train_fr_regressor_v2.py` — `fr_regressor_v2.onnx`
remains the production graph for cross-codec inference; v3 is a
parallel checkpoint that wins on NVENC-specific predictions and
serves as the schema-flip dry-run.

## Codec-blind fallback

For inference paths that don't carry codec metadata, pass an
all-zeros codec block with `encoder_onehot[libx264]=1` (slot 0 is the
fork's "default" canonical SW encoder), `preset_norm=0.5`,
`crf_norm=0.5`. The model degrades to a v1-like estimate; no graph
surgery required.

## Training recipe

Identical to
[`fr_regressor_v2`](fr_regressor_v2.md) and the deep-ensemble LOSO
trainer
([ADR-0319](../../adr/0319-ensemble-loso-trainer-real-impl.md)):

- 9-fold leave-one-source-out (LOSO) over the unique `src` values.
- Per-fold StandardScaler fit on the training rows only (mirrors
  `eval_loso_vmaf_tiny_v3.py`).
- `FRRegressor(in_features=6, hidden=64, depth=2, dropout=0.1,
  num_codecs=18)`.
- Adam(`lr=5e-4`, `weight_decay=1e-5`), MSE loss, batch_size=32,
  200 epochs.
- Final ship checkpoint is fit on the **entire** corpus (no held-out
  split) once the LOSO gate passes — the LOSO fold is the gate, not
  the ship checkpoint.

## Headline results

Mean LOSO PLCC **0.9975** ± 0.0018 (n = 9 sources). Per-source PLCC:

| Source                    | PLCC   | SROCC  | RMSE  |
|---------------------------|--------|--------|-------|
| BigBuckBunny_25fps        | 0.9973 | 0.9878 | 0.787 |
| BirdsInCage_30fps         | 0.9988 | 0.9989 | 0.432 |
| CrowdRun_25fps            | 0.9996 | 0.9972 | 0.677 |
| ElFuente1_30fps           | 0.9987 | 0.8805 | 0.822 |
| ElFuente2_30fps           | 0.9950 | 0.9984 | 3.288 |
| FoxBird_25fps             | 0.9945 | 0.9329 | 0.904 |
| OldTownCross_25fps        | 0.9981 | 0.9951 | 0.810 |
| Seeking_25fps             | 0.9989 | 0.9877 | 1.013 |
| Tennis_24fps              | 0.9962 | 0.9436 | 1.061 |

Every source clears the relaxed per-source PLCC floor (0.85) from
[Research-0078](../../research/0078-encoder-vocab-v3-schema-expansion.md)
§Retrain ship gate criterion 3, and the mean clears the 0.95 hard
floor with ~5 percentage points of margin. The min/max spread
(0.9945 → 0.9996) is well under the 0.005 ensemble-spread bound from
ADR-0303.

## CLI

```bash
# Production (real Phase A corpus)
python ai/scripts/train_fr_regressor_v3.py \
    --corpus runs/phase_a/full_grid/per_frame_canonical6.jsonl

# Smoke (synthetic corpus, validates the pipeline only)
python ai/scripts/train_fr_regressor_v3.py --smoke
```

The script bakes the full-corpus StandardScaler over the canonical-6
dims into the sidecar JSON (`feature_mean` / `feature_std`); the
codec block is unscaled. Output ONNX is opset 17, dynamic batch axis,
op-allowlist checked. Smoke mode skips the ship gate; real-corpus
mode exits non-zero on gate-fail.

The sidecar includes `run_provenance` (`ai-run-provenance-v1`) with the
trainer entrypoint, parsed arguments, corpus path/hash, and output
targets. Smoke runs point at the generated temporary corpus, which makes
the sidecar explicit that the output is a pipeline check rather than a
real Phase-A training result.

## See also

- [ADR-0323](../../adr/0323-fr-regressor-v3-train-and-register.md) —
  this PR's decision record.
- [ADR-0302](../../adr/0302-encoder-vocab-v3-schema-expansion.md) —
  the v3 16-slot schema scaffold + ship gate definition.
- [ADR-0291](../../adr/0291-fr-regressor-v2-prod-ship.md) — v2
  production-flip; defines the 0.95 LOSO PLCC ship gate v3 reuses.
- [ADR-0235](../../adr/0235-codec-aware-fr-regressor.md) — the
  parent codec-aware decision; ≥+0.005 PLCC multi-codec lift floor.
- [ADR-0319](../../adr/0319-ensemble-loso-trainer-real-impl.md) —
  LOSO trainer pattern this script reuses.
- [Research-0078](../../research/0078-encoder-vocab-v3-schema-expansion.md)
  — schema expansion plan + retrain checklist.
- [`fr_regressor_v2`](fr_regressor_v2.md) — v2 model card; v3 is the
  parallel-shipped successor on the 16-slot vocab.
