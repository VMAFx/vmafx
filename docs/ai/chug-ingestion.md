<!-- markdownlint-disable MD060 -->
# CHUG UGC-HDR ingestion

`ai/scripts/chug_to_corpus_jsonl.py` ingests the CHUG UGC-HDR dataset
into the fork's MOS-corpus JSONL shape.

## What CHUG Is

CHUG is the Crowdsourced User-Generated HDR Video Quality Dataset:
5,992 bitrate-ladder HDR videos derived from 856 UGC-HDR references,
with AMT subjective ratings and portrait/landscape coverage.

Repository: <https://github.com/shreshthsaini/CHUG>
Paper DOI: <https://doi.org/10.1109/ICIP55913.2025.11084488>

## License Posture

The CHUG README badge says CC BY-NC 4.0, while `license.txt` contains
Creative Commons Attribution-NonCommercial-ShareAlike 4.0 text. Treat
CHUG as non-commercial/share-alike research data until that mismatch is
clarified. Do not commit CHUG CSVs, videos, JSONL rows, feature caches,
or local trained heads.

## Quick Start

```bash
mkdir -p .workingdir2/chug
curl -L https://raw.githubusercontent.com/shreshthsaini/CHUG/master/chug.csv \
  -o .workingdir2/chug/manifest.csv
curl -L https://raw.githubusercontent.com/shreshthsaini/CHUG/master/chug-video.txt \
  -o .workingdir2/chug/chug-video.txt

PYTHONPATH=ai/src python ai/scripts/chug_to_corpus_jsonl.py --chug-dir .workingdir2/chug
```

The default run caps at `--max-rows 500`. Use `--full` for all 5,992
manifest rows:

```bash
PYTHONPATH=ai/src python ai/scripts/chug_to_corpus_jsonl.py \
  --chug-dir .workingdir2/chug \
  --output .workingdir2/chug/chug.jsonl \
  --full \
  --verbose
```

The run is resumable through `.workingdir2/chug/.download-progress.json`.
The adapter downloads each MP4 via `curl`, probes it with `ffprobe`,
deduplicates by SHA-256, and appends JSONL rows.

The source adapter also writes `.workingdir2/chug/chug.manifest.json` by
default (or `<output>.manifest.json` when `--output` changes). The sidecar
records the CHUG root, manifest CSV, row caps, written/skipped/dedup counters,
and ADR-0661 `run_provenance`; pass `--manifest-out PATH` to place it in a
dated experiment bundle.

## Output Schema

The common MOS-corpus fields match [mos-corpora.md](mos-corpora.md):
`src`, `src_sha256`, geometry, `mos`, `mos_std_dev`, `corpus`,
`corpus_version`, and ingest timestamp.

CHUG-specific fields are also preserved:

| Field | Meaning |
|---|---|
| `mos_raw_0_100` | Source `mos_j` value from CHUG's 0-100 MOS axis. |
| `chug_video_id` | Hashed video ID used in the S3 URL. |
| `chug_ref` | Reference flag from the CHUG CSV. |
| `chug_bitladder` | Bitrate-ladder label such as `360p_0.2M_`. |
| `chug_resolution` | Manifest resolution label. |
| `chug_bitrate_label` | Manifest bitrate label. |
| `chug_orientation` | Portrait / landscape label. |
| `chug_framerate_manifest` | Manifest framerate before ffprobe correction. |
| `chug_content_name` | Source content name from the CHUG CSV. |
| `chug_height_manifest`, `chug_width_manifest` | Manifest geometry. |

CHUG's source MOS is 0-100. The adapter maps trainer-facing `mos` onto
`[1, 5]` with:

```text
mos = 1 + 4 * mos_raw_0_100 / 100
```

The raw value remains available for future aggregation paths that use a
0-100 MOS axis directly.

## Local Baseline Training

Once `chug.jsonl` exists, materialise feature rows before training:

```bash
PYTHONPATH=ai/src python ai/scripts/chug_extract_features.py \
  --input .workingdir2/chug/chug.jsonl \
  --output .workingdir2/chug/chug_features.jsonl \
  --clips-dir .workingdir2/chug/clips \
  --cache-dir .workingdir2/chug/feature-cache \
  --split-manifest .workingdir2/chug/chug_splits.json \
  --audit-output .workingdir2/chug/chug_hdr_audit.json \
  --vmaf-bin core/build-cpu/tools/vmaf \
  --feature-set canonical \
  --full
```

The materialiser pairs each distorted ladder row with the matching
`chug_content_name` reference row, decodes both clips as 10-bit 4:2:0
YUV, scales the distorted side to the reference geometry, runs libvmaf,
and writes clip-level feature aggregates. The trainer-facing feature row
contains the canonical bare feature names (`adm2`, `vif_scale0` ...
`motion2`) as means, plus `<feature>_mean`, `<feature>_p10`,
`<feature>_p90`, and `<feature>_std` columns. The CHUG trainer uses
those temporal aggregates by default rather than throwing them away.
Each row also carries ffprobe-derived HDR/display metadata for both
input clips:

- `feature_ref_*` describes the matched reference clip.
- `feature_dis_*` describes the distorted ladder clip.
- The suffixes are `codec_name`, `pix_fmt`, `color_transfer`,
  `transfer_class`, `color_primaries`, `color_space`, `color_range`,
  `max_content_nits`, and `max_average_nits`.

`transfer_class` is normalized to `pq`, `hlg`, `sdr`, or `unknown` so
training scripts can consume a stable categorical field even when
ffprobe reports vendor-specific transfer strings. Missing static
metadata stays explicit as `unknown` or `null`; the materialiser does
not infer panel capability from the clip alone.

The same decode pass also emits cheap luma-domain visual-signal
primitives for both sides:

- `feature_ref_luma_std` / `feature_dis_luma_std` — luma contrast proxy.
- `feature_ref_sharpness_laplacian_var` / `feature_dis_sharpness_laplacian_var`
  — Laplacian-variance sharpness proxy; lower values usually mean blur,
  while high values may be either detail or noise.
- `feature_ref_highfreq_abs_mean` / `feature_dis_highfreq_abs_mean` —
  high-frequency texture/grain proxy from neighboring-pixel differences.
- `feature_ref_noise_lap_mad` / `feature_dis_noise_lap_mad` —
  robust high-pass residual proxy for noise/grain.
- `feature_delta_*` — distorted-minus-reference deltas for the four
  fields above.

These are intentionally low-cost diagnostics, not a no-reference VQA
model. They make the CHUG feature table aware of blur/noise/grain axes
that libvmaf's canonical six features do not expose directly.

The materialiser assigns train/validation/test splits at
`chug_content_name` granularity, not at row granularity. Every bitrate
ladder variant for a source content therefore stays in one split, which
prevents reference-content leakage across validation. The default policy
is deterministic `80/10/10` BLAKE2s hashing with seed `chug-hdr-v1`; use
`--split train`, `--split val`, or `--split test` to materialise one
partition, and `--split-manifest` to write the local content-to-split
map.

`--audit-output` writes a local ffprobe HDR metadata audit before
feature extraction. The audit records row counts, probe failures,
transfer-characteristic counts (`pq`, `hlg`, `sdr`, `unknown`),
primaries, pix-fmt distribution, split row counts, and malformed HDR
rows where PQ/HLG is signalled without BT.2020 primaries. This is the
first check to run before using a CHUG feature file for HDR experiments.
The audit is a corpus-level preflight; the per-row `feature_ref_*` and
`feature_dis_*` fields are the model-facing copy preserved in the
training rows.

Both `--split-manifest` and `--audit-output` include the shared
`run_provenance` block from ADR-0661. That block records the
`chug_extract_features.py` entrypoint, argv, parsed arguments, input
JSONL, clip/cache directories, VMAF binary, and output targets. Keep
those local JSON files with CHUG training artifacts so a later model-card
or held-out validation pass can prove which feature extraction command
created the split and HDR preflight evidence.

Train against the feature rows:

```bash
python ai/scripts/train_chug_hdr_mos_head.py \
  --feature-jsonl .corpus/chug/training/fr_canonical_shards/output/shard_00.features.jsonl \
  --feature-jsonl .corpus/chug/training/fr_canonical_shards/output/shard_01.features.jsonl \
  --feature-jsonl .corpus/chug/training/fr_canonical_shards/output/shard_02.features.jsonl \
  --feature-jsonl .corpus/chug/training/fr_canonical_shards/output/shard_03.features.jsonl \
  --feature-jsonl .corpus/chug/training/fr_canonical_shards/output/shard_04.features.jsonl \
  --feature-jsonl .corpus/chug/training/fr_canonical_shards/output/shard_05.features.jsonl \
  --feature-jsonl .corpus/chug/training/fr_canonical_shards/output/shard_06.features.jsonl \
  --feature-jsonl .corpus/chug/training/fr_canonical_shards/output/shard_07.features.jsonl \
  --model-id chug_hdr_mos_head_v1 \
  --out-onnx .workingdir2/chug/chug_hdr_mos_head_v1.onnx \
  --out-card .workingdir2/chug/chug_hdr_mos_head_v1_card.md \
  --out-manifest .workingdir2/chug/chug_hdr_mos_head_v1.json
```

When all canonical shards live under
`.corpus/chug/training/fr_canonical_shards/output/`, the shorter form is
equivalent:

```bash
python ai/scripts/train_chug_hdr_mos_head.py
```

By default the wrapper trains with `--feature-schema chug-hdr-wide-v1`.
That 34-column schema contains:

- the canonical-6 means: `adm2`, `vif_scale0..3`, `motion2`;
- p10, p90, and standard-deviation temporal summaries for each
  canonical feature;
- CHUG HDR ladder metadata: bitrate in Mbps, portrait/landscape flag,
  reference-row flag, distorted/reference geometry, duration, feature
  frame count, and bit depth.

Use the legacy 11-column KonViD layout only for ablation or regression
comparison:

```bash
python ai/scripts/train_chug_hdr_mos_head.py --feature-schema konvid-v1
```

For display-aware HDR experiments, pass a target panel profile:

```bash
cat > .workingdir2/chug/display-profile.json <<'JSON'
{
  "display": {
    "peak_nits": 1000,
    "black_nits": 0.005,
    "ambient_lux": 25,
    "bt2020_coverage": 0.72,
    "p3_coverage": 0.95,
    "panel_type": "OLED",
    "local_dimming": false,
    "tone_mapping": "HDR10"
  }
}
JSON

python ai/scripts/train_chug_hdr_mos_head.py \
  --display-profile-json .workingdir2/chug/display-profile.json
```

When `--display-profile-json` is supplied and `--feature-schema` is
omitted, the wrapper selects `chug-hdr-display-v1`. That 45-column
schema appends normalized target-display features to
`chug-hdr-wide-v1`: peak luminance, black level, log contrast ratio,
ambient lux, BT.2020/P3 coverage, OLED/QLED/LCD panel flags, local
dimming, and dynamic tone-mapping. The profile is recorded in the
manifest under `display_profile` with a sha256 of the source JSON.

The generated manifest also includes `run_provenance`. For CHUG runs,
`entrypoint` is `ai/scripts/train_chug_hdr_mos_head.py`, `shared_trainer`
is the shared KonViD MOS trainer, and `inputs` includes every
`--feature-jsonl`, optional feature parquet, and optional display-profile
JSON path with file hashes when those files exist.

If a future HDR corpus row already carries display fields, row-local
values win and the profile only fills missing display features. That
keeps multi-display datasets usable while still letting CHUG runs bind
their MOS head to a target consumer panel.

When feature rows carry the `split` column emitted by
`chug_extract_features.py`, `train_chug_hdr_mos_head.py` uses that
content-level split for validation instead of creating random k-folds.
The exported local checkpoint is trained on the `train` partition only,
leaving `val` / `test` rows held out for calibration and reporting.
`--model-id chug_hdr_mos_head_v1` keeps the local manifest honest: this
is a CHUG HDR subjective-MOS model, not the committed SDR KonViD MOS
head.

This is a baseline unlock, not a final HDR model. The CHUG feature rows
carry full-reference libvmaf features and subjective HDR MOS labels.
They are the fork's interim HDR signal until Netflix ships an HDR VMAF
model; do not treat the current SDR `vmaf_v0.6.1` teacher as an HDR
ground truth.

## Local FULL_FEATURES Experiments

For CHUG full-reference training, prefer the JSONL emitted by
`ai/scripts/chug_extract_features.py`. Do **not** point the generic
FR-from-NR `extract_k150k_features.py` adapter at CHUG bitrate ladders
for normal training: CHUG has explicit reference/distorted pairs, and the
generic adapter scores each clip against itself unless it is used for a
deliberate identity-pair study.

If you already have a metadata-enriched FULL_FEATURES parquet from a
correct full-reference CHUG run, the trainer can consume it directly:

```bash
python ai/scripts/train_chug_hdr_mos_head.py \
  --feature-parquet .workingdir2/chug/training/full_features_chug.parquet \
  --out-onnx .workingdir2/chug/chug_full_features_mos_head.onnx \
  --out-card .workingdir2/chug/chug_full_features_mos_head_card.md \
  --out-manifest .workingdir2/chug/chug_full_features_mos_head.json
```

`train_chug_hdr_mos_head.py` forwards to the shared MOS trainer, which
reads both bare trainer columns (`adm2`) and
FULL_FEATURES aggregate columns (`adm2_mean`). If the parquet was written
with `--metadata-jsonl`, the trainer also honors the `split` column so the
CHUG content-level holdout survives the parquet path.

If a long-running extraction was started without `--metadata-jsonl`, do
not rerun the feature pass just to recover split metadata. Enrich the
finished parquet in place from `chug.jsonl`:

```bash
python ai/scripts/enrich_k150k_parquet_metadata.py \
  --features-parquet .workingdir2/chug/training/full_features_chug.parquet \
  --metadata-jsonl .workingdir2/chug/chug.jsonl
```

The enrichment utility matches rows by `clip_name`, fills missing CHUG
side columns, and leaves existing feature/MOS columns untouched unless
`--overwrite-metadata` is passed. It writes
`<out>.manifest.json` by default, or
`<features-parquet>.manifest.json` for in-place enrichment, with row-match
counts, the metadata keys that were available, and the shared
`run_provenance` block. Keep that manifest with the enriched parquet so a
later HDR model card can distinguish a freshly extracted table from a
post-enriched recovery artifact.
