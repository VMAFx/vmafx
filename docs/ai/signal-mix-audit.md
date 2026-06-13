<!-- markdownlint-disable MD060 -->
# Signal-mix audit

`ai/scripts/signal_mix_audit.py` audits already-extracted feature tables and
answers three operator questions:

1. Which quality dimensions are represented by the current columns?
2. Which columns are redundant or complementary against the chosen target?
3. Which metric families are completely missing or present only as flat/NaN
   data?

The audit is advisory. It does not extract libvmaf features, train checkpoints,
or change corpus files. Use it before retraining models or changing encoder
profile defaults so missing signal families are visible in the report instead
of buried in stale planning notes.

## Inputs

Inputs can be parquet, JSONL/NDJSON, or a JSON list of row objects. Repeat
`--input` for multi-corpus audits:

```bash
.venv/bin/python ai/scripts/signal_mix_audit.py \
  --input netflix=runs/full_features_netflix_refresh_20260520.parquet \
  --input ugc=runs/full_features_ugc_refresh_20260520.parquet \
  --input bvi=runs/full_features_bvi_dvc_D_refresh_20260520.parquet \
  --out-json .workingdir2/signal-mix/audit.json \
  --out-md .workingdir2/signal-mix/audit.md
```

Use `LABEL=path` to make the rendered report readable. If no target is passed,
the script searches for common target columns in this order:

`vmaf`, `vmaf_score`, `mos_raw_0_100`, `mos`, `dmos`, `score`.

For a custom MOS or lab score:

```bash
.venv/bin/python ai/scripts/signal_mix_audit.py \
  --input chug=.workingdir2/chug/chug_features.jsonl \
  --target mos_raw_0_100 \
  --out-json .workingdir2/signal-mix/chug.json \
  --out-md .workingdir2/signal-mix/chug.md
```

The JSON report includes ADR-0661 `run_provenance` with the audited table
paths, parsed thresholds, argv, JSON report target, and Markdown report target.
Keep the JSON next to the Markdown when an audit drives model-mix or feature
materializer work; it is the machine-readable proof of which tables produced
the human report.

## Signal Families

The audit maps columns into broad signal families, not individual model
contracts. A column can count for more than one family when names overlap.

| Family | Examples |
|---|---|
| FR detail and motion baseline | `adm2`, `vif_scale0..3`, `motion2`, VMAF teacher columns |
| Error energy and HVS-weighted PSNR | `psnr_y`, `psnr_hvs`, `mse`, `rmse` |
| Local structural similarity | `float_ssim`, `float_ms_ssim`, `iw_ssim` |
| Texture and deep perceptual similarity | `ssimulacra2`, `lpips`, `dists`, VGG-derived signals |
| Color and chroma fidelity | `ciede2000`, `speed_chroma_*`, `psnr_cb`, `psnr_cr`, primaries |
| Banding and tone mapping | `cambi`, PQ/HLG transfer fields, clipping and tone-map fields |
| Temporal instability and scene structure | `motion*`, `speed_temporal`, `shot_*`, `transnet`, p10/p90/std pools |
| Saliency and ROI weighting | `saliency_*`, `mobilesal`, `u2net`, ROI masks |
| HDR display and panel context | bit depth, transfer/EOTF, mastering data, panel/display fields |
| Source geometry and content metadata | resolution, duration, frame rate, orientation, category/content labels |
| Codec and rate-control context | codec, encoder, preset, CRF/QP, bitrate, hardware encoder tokens |
| No-reference UGC and subjective MOS | MOS/DMOS, rating counts, UGC labels, orientation/category, `second_opinion_*` |
| Noise, grain, blur, and sharpening | noise/grain/blur/sharpness/edge columns |

The candidate-metric column in the Markdown report is intentionally broader
than the current tree. It calls out useful intersections that may not be wired
yet: U2NetP, DISTS, LPIPS, HDR-VDP, PU-PSNR/PU-SSIM, DOVER, Q-Align, FAST-VQA,
MUSIQ, CLIP-IQA, VMAF NEG, and panel metadata. When those no-reference
or subjective scorers are run out-of-tree, join their results back into the
table with [Second-opinion features](second-opinion-features.md) so the audit
sees `second_opinion_*` columns as NR/MOS evidence.

## Reading the Report

The generated Markdown has five sections:

- **Coverage Matrix**: whether each signal family is covered, weak, or missing
  per input table. `weak` means the family has matching columns but no healthy
  numeric variance at the configured finite/variance thresholds. Categorical
  metadata such as a string `codec` column therefore appears as `weak` until it
  is encoded or accompanied by numeric profile fields.
- **Strongest Target Signals**: per-column Pearson and Spearman correlation
  against the detected or requested target.
- **Complementary Intersections**: cross-family pairs that both correlate with
  the target but are not too correlated with each other. These are the most
  interesting candidates for richer model mixes.
- **Redundant Pairs**: columns above the redundancy threshold. These are useful
  for pruning or for detecting duplicate feature aliases.
- **Blind Spots**: missing and weak families plus human-readable next actions.

Defaults:

```text
redundancy threshold: |Pearson r| >= 0.95
complement threshold: |Pearson r| <= 0.70
minimum finite ratio: 0.80
```

Relax `--complement-threshold` when a small synthetic or highly structured
table has near-linear columns. Tighten it when searching for truly independent
new metrics.

## Current Use

Run this audit after feature refreshes and before model promotion. For the
2026-05-20 refresh stream, the main rows to inspect are:

- refreshed Netflix, BVI-DVC, UGC, KoNViD, K150K, and CHUG tables;
- `chug-hdr-wide-v1` outputs, because CHUG is HDR MOS rather than a Netflix SDR
  teacher target;
- saliency/U2NetP experiment outputs once they exist;
- encoder-profile tables before changing `vmaf-tune` codec defaults.

If a table says HDR display/panel context is weak, do not treat a passing CHUG
MOS gate as panel-aware. CHUG gives HDR MOS signal, but panel tuning still
requires display metadata or a deliberate fallback policy.
