<!-- markdownlint-disable MD060 -->
# LIVE-VQC corpus ingestion

LIVE Video Quality Challenge (LIVE-VQC; Sinno & Bovik, IEEE TIP 2019) is a
585-video real-world UGC dataset collected by the LIVE Lab at UT Austin.
Videos were captured on consumer smartphones and tablets across diverse
real-world scenes, providing authentic in-the-wild distortions from the
device perspective rather than from a transcoding pipeline.

This page covers acquisition, operator flags, and schema details for the
`live_vqc_to_corpus_jsonl.py` adapter (ADR-0370). For the unified family
index see [mos-corpora.md](mos-corpora.md).

## Corpus identity

| Property | Value |
|----------|-------|
| Clips | 585 |
| MOS scale | 0–100 continuous (LIVE Lab crowdsourcing framework) |
| Size (approximate) | A few GB |
| Corpus label (`corpus` field) | `live-vqc` |
| Default `corpus_version` | `live-vqc-2019` |
| License | Research use with attribution |
| Dataset page | <https://live.ece.utexas.edu/research/LIVEVQC/> |

**Citation:**
> Sinno, Z., Bovik, A. C., "Large-Scale Study of Perceptual Video Quality,"
> IEEE Transactions on Image Processing, 28(2), pp. 612–627, Feb. 2019.
> DOI: 10.1109/TIP.2018.2875341

## Acquisition

LIVE-VQC is available via the UT Austin LIVE Lab website (link above). The
dataset typically requires a short request form. The adapter does **not**
include any clips, per-clip MOS values, or derived feature caches — only the
conversion script ships in tree.

Expected local layout after extraction:

```text
.workingdir2/live-vqc/
  ├── manifest.csv        # MOS table (operator drops — see Manifest below)
  └── clips/              # video files (operator extraction)
        ├── 001.mp4
        └── ...
```

## Quick-start

```bash
# Laptop-class smoke run (200 clips, requires manifest + clips on disk):
python ai/scripts/live_vqc_to_corpus_jsonl.py
#    → .workingdir2/live-vqc/live_vqc.jsonl

# Full-corpus ingestion (all 585 clips):
python ai/scripts/live_vqc_to_corpus_jsonl.py --full
#    → .workingdir2/live-vqc/live_vqc.jsonl

# Custom directory:
python ai/scripts/live_vqc_to_corpus_jsonl.py \
    --live-vqc-dir /data/live-vqc \
    --output /data/live-vqc/live_vqc.jsonl
```

The run is **resumable** — Ctrl-C and re-run; the
`.download-progress.json` sidecar tracks per-clip state.

## Manifest format

Two shapes are accepted and auto-detected:

### 1. Canonical two-column headerless (minimal MOS export)

The LIVE-VQC MOS spreadsheet can be exported as a headerless
`<filename>, <mos>` CSV. Drop it at `<live-vqc-dir>/manifest.csv`:

```text
001.mp4,45.23
002.mp4,72.18
003.mp4,61.05
```

In this shape `mos_std_dev` and `n_ratings` default to `0.0` / `0` (the
export does not include inter-rater spread).

### 2. Standard adapter CSV (named-column header)

Alternatively, produce a standard CSV matching the LSVQ / KonViD-150k
header convention:

```text
name,url,mos,sd,n
001.mp4,https://...,45.23,8.1,30
002.mp4,https://...,72.18,6.4,28
```

Column aliases accepted for each field:

| Field | Aliases |
|-------|---------|
| filename | `name`, `video_name`, `filename`, `file_name` |
| URL | `url`, `download_url`, `video_url` |
| MOS | `mos`, `MOS`, `mos_score` |
| MOS SD | `sd`, `SD`, `mos_std`, `mos_std_dev`, `SD_MOS` |
| n ratings | `n`, `ratings`, `num_ratings`, `n_ratings` |

If the URL column is present, the adapter attempts to download missing clips
via curl (resumable, with a 120-second per-clip timeout).

## CLI flags

| Flag | Default | Description |
|------|---------|-------------|
| `--live-vqc-dir` | `.workingdir2/live-vqc/` | Local working directory |
| `--manifest-csv` | `<dir>/manifest.csv` | MOS manifest path |
| `--clips-subdir` | `clips` | Sub-directory for video files |
| `--output` | `<dir>/live_vqc.jsonl` | Output JSONL path |
| `--manifest-out` | `<output>.manifest.json` | Replay manifest JSON sidecar |
| `--max-rows` | `200` | Cap on rows ingested (laptop-class subset) |
| `--full` | off | Ingest the entire manifest; overrides `--max-rows` |
| `--corpus-version` | `live-vqc-2019` | Version string baked into each row |
| `--ffprobe-bin` | `ffprobe` | ffprobe binary path / env `$FFPROBE_BIN` |
| `--curl-bin` | `curl` | curl binary path / env `$CURL_BIN` |
| `--attrition-warn-threshold` | `0.10` | Warn if download failures exceed this fraction |
| `--download-timeout-s` | `120` | Per-clip curl `--max-time` seconds |
| `--log-level` | `INFO` | Logging level (`DEBUG` / `INFO` / `WARNING` / `ERROR`) |
| `--progress-path` | `<dir>/.download-progress.json` | Resumable download state; delete to retry failures |

The replay manifest records the LIVE-VQC working directory, manifest path, row
cap, attrition counters, effective corpus version, and ADR-0661
`run_provenance`.

## Output schema

Every row in the output JSONL follows the corpus_v3 schema (ADR-0366):

```jsonc
{
  "src":               "001.mp4",
  "src_sha256":        "<64-hex>",
  "src_size_bytes":    4321000,
  "width":             1920,
  "height":            1080,
  "framerate":         30.0,
  "duration_s":        6.0,
  "pix_fmt":           "yuv420p",
  "encoder_upstream":  "h264",
  "mos":               63.4,          // native 0–100 scale — NOT normalised
  "mos_std_dev":       8.2,           // 0.0 if two-column CSV consumed
  "n_ratings":         30,            // 0 if two-column CSV consumed
  "corpus":            "live-vqc",
  "corpus_version":    "live-vqc-2019",
  "ingested_at_utc":   "2026-05-09T12:00:00+00:00"
}
```

**MOS scale note:** LIVE-VQC uses a 0–100 continuous scale, not the 1–5 ACR
Likert scale used by KonViD-1k / KonViD-150k / LSVQ. When combining corpora
use `ai/scripts/aggregate_corpora.py`, which normalises each shard to a
common axis before the trainer consumes it. See
[multi-corpus-aggregation.md](multi-corpus-aggregation.md).

## Integrating with the unified training pipeline

```bash
python ai/scripts/aggregate_corpora.py \
    --inputs .workingdir2/konvid-150k/konvid_150k.jsonl \
             .workingdir2/lsvq/lsvq.jsonl \
             .workingdir2/live-vqc/live_vqc.jsonl \
    --output .workingdir2/aggregated/unified_corpus.jsonl
```

## License and redistribution

LIVE-VQC is available for research use with attribution. No clips, MOS values,
or derived features are committed to this repository (per ADR-0370). ONNX
weights trained on LIVE-VQC data travel with the Sinno & Bovik 2019 citation
in their model-card sidecar.

## Related

- [mos-corpora.md](mos-corpora.md) — unified family index
- [multi-corpus-aggregation.md](multi-corpus-aggregation.md) — normalisation
- [ADR-0370](../adr/0370-live-vqc-corpus-ingestion.md) — decision record
