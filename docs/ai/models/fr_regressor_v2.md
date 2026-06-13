# FR regressor v2 — codec-aware (vmaf-tune corpus consumer)

`fr_regressor_v2` — codec-conditioned successor to
[`fr_regressor_v1`](fr_regressor_v1.md). Maps a 6-D canonical libvmaf
feature vector plus an 8-D codec block to a VMAF teacher score.
Trained on the JSONL corpus emitted by `vmaf-tune corpus` (Phase A,
[ADR-0237](../../adr/0237-quality-aware-encode-automation.md)).

> **Status: production checkpoint.** `model/tiny/registry.json`
> registers `fr_regressor_v2.onnx` with `smoke: false`, SHA-256 pin
> `67934b0b61c73eb852d84ffb34e3333756e8da2530179ecc830336133e63e69e`,
> and an in-sample PLCC of 0.9794 on the vmaf-tune Phase-A JSONL
> corpus. The old scaffold-only card text is superseded; the follow-up
> line is now the v3 16-slot vocabulary / LOSO production checkpoint,
> documented in [`fr_regressor_v3.md`](fr_regressor_v3.md).

## Inputs

Two named tensors, dynamic batch axis:

- **`features`**, shape `(N, 6)` — canonical-6 libvmaf features
  (StandardScaler-normalised at training time using the mean/std
  baked into the sidecar JSON):

  | Index | Feature        |
  |-------|----------------|
  | 0     | `adm2`         |
  | 1     | `vif_scale0`   |
  | 2     | `vif_scale1`   |
  | 3     | `vif_scale2`   |
  | 4     | `vif_scale3`   |
  | 5     | `motion2`      |

- **`codec`**, shape `(N, 8)` — codec block, **not** normalised
  (already in `[0, 1]`):

  | Index | Slot                               |
  |-------|------------------------------------|
  | 0     | `encoder_onehot[libx264]`          |
  | 1     | `encoder_onehot[libx265]`          |
  | 2     | `encoder_onehot[libsvtav1]`        |
  | 3     | `encoder_onehot[libvvenc]`         |
  | 4     | `encoder_onehot[libvpx-vp9]`       |
  | 5     | `encoder_onehot[unknown]`          |
  | 6     | `preset_norm`  (preset ordinal / 9)|
  | 7     | `crf_norm`     (CRF / 63)          |

  Encoder vocabulary is closed and ordered — index 0..5 is
  load-bearing; bumping the vocabulary requires a re-train. The
  `unknown` bucket lets corpora without codec metadata pass an
  all-zeros + `unknown=1` vector and degrade gracefully.

  CRF normalised by **63** — the union upper bound across all
  supported encoders (libsvtav1 / libvpx-vp9 max). x264 / x265 use
  CRF up to 51; values above their per-encoder max are clipped at
  read time.

  Preset ordinal table per encoder lives in
  [`ai/scripts/train_fr_regressor_v2.py`](../../../ai/scripts/train_fr_regressor_v2.py)
  (`PRESET_ORDINAL`); the canonical 0..9 scale carries the
  speed-quality direction consistently across encoders. libsvtav1's
  numeric 0..13 presets are squashed to 0..9.

## Output

`score`, shape `(N,)` — a scalar VMAF-aligned quality score per
sample, same MOS range as v1 (typically `[0, 100]`).

## Codec-blind fallback

For inference paths that don't carry codec metadata, pass an
all-zeros codec vector with `encoder_onehot[unknown]=1` and
`preset_norm=0.5`, `crf_norm=0.5`. The model degrades to a v1-like
estimate; no graph surgery required.

## Training corpus

vmaf-tune Phase A JSONL (`tools/vmaf-tune/src/vmaftune/corpus.py`).
One row per `(source, encoder, preset, crf)` cell with
`schema_version=1`. The trainer reads the JSONL row-by-row; the
canonical-6 features come from each row's measured libvmaf feature
payload when present, with compatibility aliases for historical corpus
runs. `--smoke` remains available for CI/load-path validation, but the
committed `fr_regressor_v2.onnx` is the production export recorded in
the registry.

## CLI

```bash
# Smoke (synthetic corpus, validates the pipeline only)
python ai/scripts/train_fr_regressor_v2.py --smoke

# Production (real Phase A corpus)
python ai/scripts/train_fr_regressor_v2.py \
    --corpus runs/vmaf_tune_corpus.jsonl \
    --epochs 30
```

The script bakes the StandardScaler over the canonical-6 dims into
the sidecar JSON (`feature_mean` / `feature_std`); the codec block
is unscaled. Output ONNX is opset 17, dynamic batch axis, op-allowlist
checked.

The sidecar and `--metrics-out` JSON include `run_provenance`
(`ai-run-provenance-v1`): trainer entrypoint, command arguments,
real-corpus path/hash or `synthetic-smoke`, and output targets. Use this
block when comparing refreshed codec-aware runs so stale Phase-A
corpora are visible without reverse-engineering shell history.

## See also

- [ADR-0272](../../adr/0272-fr-regressor-v2-codec-aware-scaffold.md)
  — original scaffold decision; this card now reflects the promoted
  production checkpoint.
- [ADR-0235](../../adr/0235-codec-aware-fr-regressor.md) — the
  parent codec-aware decision.
- [ADR-0237](../../adr/0237-quality-aware-encode-automation.md) —
  vmaf-tune Phase A (the corpus producer).
- [ADR-0249](../../adr/0249-fr-regressor-v1.md) —
  `fr_regressor_v1` baseline.
- [Research-0054](../../research/0058-fr-regressor-v2-feasibility.md)
  — feasibility digest, including the open question on production
  corpus diversity.
- [`fr_regressor_v2_codec_aware.md`](fr_regressor_v2_codec_aware.md)
  — superseded ADR-0235-era design card (canonical-9 / FULL_FEATURES
  path). The shipped model is `fr_regressor_v2` (this card), not a
  separate `fr_regressor_v2_codec_aware.onnx`.
