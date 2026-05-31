<!-- markdownlint-disable MD060 -->
# Waterloo IVC 4K-VQA → MOS-corpus JSONL ingestion

The fork's `nr_metric_v1` tiny no-reference VQA model is
trained on a union of MOS-corpus shards. This page documents
the Waterloo IVC 4K-VQA shard ingestion adapter
(`ai/scripts/waterloo_ivc_to_corpus_jsonl.py`, ADR-0369).

## What Waterloo IVC 4K-VQA is

Waterloo IVC 4K-VQA — the University of Waterloo Image and
Vision Computing Laboratory's 4K Video Quality Database
(Li, Duanmu, Liu, Wang; ICIAR 2019) — is a
controlled-subjective-study video-quality corpus with
2160p coverage. Twenty pristine 4K source sequences are
re-encoded with five contemporary codecs at three
resolutions and four distortion levels, yielding 1 200
distorted clips with per-clip MOS.

- **Sources**: 20 pristine 2160p sequences.
- **Encoders**: H.264/AVC, H.265/HEVC, VP9, AVS2, AV1.
- **Resolutions**: 540p / 1080p / 2160p.
- **Distortion levels**: 4 per (encoder, resolution).
- **Total distorted clips**: 1 200.
- **MOS scale**: 0–100 native (not 1–5 Likert; see
  "Cross-corpus MOS scale caveat" below).
- **Dataset card**:
  <https://ivc.uwaterloo.ca/database/4KVQA.html>.
- **Archive base**:
  <https://ivc.uwaterloo.ca/database/4KVQA/201908/>.
- **Licence**: Permissive academic — attribution required,
  no NDA, no password gate, no registration form.

## When to run the adapter

Run this adapter when you want to (re-)build the
Waterloo IVC 4K-VQA MOS-corpus JSONL shard the trainer
consumes alongside BVI-DVC, KonViD-150k, and LSVQ.

## Prerequisites

1. Multi-TB free disk under `.workingdir2/waterloo-ivc-4k/`
   for the whole-corpus run, or ~few-hundred GB for the
   laptop-class default.
2. `curl` and `ffprobe` on `$PATH`.
3. The scores table from
   <https://ivc.uwaterloo.ca/database/4KVQA/201908/scores.txt>
   dropped at `.workingdir2/waterloo-ivc-4k/manifest.csv`
   (or pass `--manifest-csv` to point elsewhere).
4. The bulk archives (Sources, H264, HEVC, VP9, AVS2 / AV1
   split-4-part) extracted to
   `.workingdir2/waterloo-ivc-4k/clips/`. The canonical
   `scores.txt` carries no per-clip URL column, so the
   adapter expects clips to be staged on disk before it
   runs (the upstream's bulk-archive distribution model).

## Quick start (laptop-class subset)

```bash
# Drop the upstream scores.txt and the extracted bulk
# archives at the default location, then:
python ai/scripts/waterloo_ivc_to_corpus_jsonl.py
# → reads .workingdir2/waterloo-ivc-4k/manifest.csv
# → caps at the first --max-rows=100 clips (default)
# → probes each clip via ffprobe
# → writes .workingdir2/waterloo-ivc-4k/waterloo_ivc_4k.jsonl
```

## Whole-corpus ingestion

```bash
python ai/scripts/waterloo_ivc_to_corpus_jsonl.py --full
```

This disables the `--max-rows` cap. Working set is
multi-TB end-to-end on the canonical 1 200-clip distorted
set + 20 pristine 4K sources.

## Output schema

One JSON object per line in
`.workingdir2/waterloo-ivc-4k/waterloo_ivc_4k.jsonl`:

```jsonc
{
  "src":               "HEVC_1_540p_1.yuv",
  "src_sha256":        "<hex>",
  "src_size_bytes":    1234567,
  "width":             3840,
  "height":            2160,
  "framerate":         30.0,
  "duration_s":        10.0,
  "pix_fmt":           "yuv420p10le",
  "encoder_upstream":  "hevc",
  "mos":               18.21,
  "mos_std_dev":       0.0,
  "n_ratings":         0,
  "corpus":            "waterloo-ivc-4k",
  "corpus_version":    "waterloo-ivc-4k-201908",
  "ingested_at_utc":   "2026-05-08T10:00:00+00:00"
}
```

The schema is byte-identical to the KonViD-150k Phase 2
and LSVQ adapters' modulo the `corpus` and
`corpus_version` literals. MOS is recorded verbatim on
the Waterloo-native 0–100 scale (no rescaling at ingest
time); see the cross-corpus caveat below.

The canonical headerless `scores.txt` carries neither a
MOS standard deviation nor a rating count — those columns
are absent upstream. The adapter records `mos_std_dev =
0.0` and `n_ratings = 0` for canonical-shape rows; if an
operator pre-mangles the scores file into the standard
LSVQ-shape CSV with explicit `sd` / `n` columns, those
round-trip verbatim.

## Cross-corpus MOS scale caveat

Waterloo IVC 4K-VQA records MOS on **0–100 raw**, while
KonViD-150k and LSVQ are on **1–5 Likert**. The adapter
records the score verbatim on its native scale (no
ingest-time rescaling), matching the policy of LSVQ /
KonViD-150k. The convention is:

- `corpus = "waterloo-ivc-4k"` rows carry MOS on 0–100.
- `corpus = "konvid-150k"` / `"lsvq"` rows carry MOS on
  1–5.

A trainer-side per-corpus normaliser (e.g. mapping
0–100 → 1–5 via `1 + 4·(x/100)`) is the clean fix and
lands in a separate PR. See ADR-0369 §Consequences and
[`docs/rebase-notes.md`](../rebase-notes.md).

## Manifest CSV shapes

The adapter accepts two manifest shapes; auto-detection
sniffs the first row.

### Canonical headerless 5-tuple (upstream `scores.txt`)

The upstream `scores.txt` is **headerless**, with five
comma-separated columns:

```text
encoder, video_number, resolution, distortion_level, mos
```

Sample rows verbatim:

```text
HEVC, 1, 540p, 1, 18.21
HEVC, 1, 540p, 2, 39.46
HEVC, 1, 540p, 3, 50.23
HEVC, 1, 540p, 4, 77.26
HEVC, 1, 1080p, 1,  7.61
```

The adapter synthesises the on-disk filename via the
convention `{encoder}_{video_number}_{resolution}_
{distortion}.<suffix>` (default `.yuv`) and looks the
clip up under `clips/`.

### Standard LSVQ / KonViD-150k header

When an operator pre-mangles `scores.txt` into a
named-column CSV with the LSVQ-shape header
(`name,url,mos,sd,n`), the adapter parses it through the
standard branch. Aliases:

| Logical column | Recognised header spellings |
|---|---|
| filename | `name`, `video_name`, `filename`, `file_name` |
| URL (optional) | `url`, `download_url`, `video_url` |
| MOS | `mos`, `MOS`, `mos_score` |
| MOS std-dev | `sd`, `SD`, `mos_std`, `mos_std_dev`, `SD_MOS` |
| rating count | `n`, `ratings`, `num_ratings`, `n_ratings` |

## Operator flags

```text
--waterloo-ivc-dir PATH       Working dir
                              (default: .workingdir2/waterloo-ivc-4k/)
--manifest-csv PATH           Path to manifest
                              (default: <dir>/manifest.csv)
--progress-path PATH          Resumable state file
                              (default: <dir>/.download-progress.json)
--clips-subdir NAME           Subdir for clips (default: clips)
--clip-suffix EXT             Default file suffix (default: .yuv)
--output PATH                 Output JSONL
                              (default: <dir>/waterloo_ivc_4k.jsonl)
--manifest-out PATH           Replay manifest JSON sidecar
                              (default: <output>.manifest.json)
--ffprobe-bin BIN             ffprobe binary
                              (default: $FFPROBE_BIN or ffprobe)
--curl-bin BIN                curl binary
                              (default: $CURL_BIN or curl)
--corpus-version STR          Dataset version
                              (default: waterloo-ivc-4k-201908)
--attrition-warn-threshold F  Advisory failure-rate floor
                              (default: 0.10)
--download-timeout-s N        Per-clip curl --max-time
                              (default: 120)
--max-rows N                  Row cap
                              (default: 100; laptop-class subset)
--full                        Disable --max-rows cap; ingest whole CSV
--log-level LEVEL             DEBUG / INFO / WARNING / ERROR
```

The replay manifest records the Waterloo working directory, manifest/progress
paths, row cap, attrition counters, effective corpus version, and ADR-0661
`run_provenance`.

## Failure handling

- **Download failure** (HTTP 404 / 410, curl spawn
  failure, empty body, missing URL on canonical-shape
  rows): logged with the reason, persisted to the
  progress file as `state: "failed"`, run continues.
  Re-runs honour the non-retry contract — to retry,
  delete the entry from the progress file or delete the
  whole file.
- **ffprobe failure** ("broken-clip"): logged, run
  continues, no row emitted. Distinct from
  download-failed in the summary line.
- **Attrition WARNING**: when the download-failed
  fraction exceeds `--attrition-warn-threshold` (default
  10 %), an advisory WARNING is logged. The run still
  completes. For canonical-shape (no-URL) operations the
  threshold is effectively a "missing-clips" warning —
  every "download failure" really means a clip is not
  staged on disk.

## License & redistribution

Waterloo IVC 4K-VQA is published under the Image and
Vision Computing Laboratory permissive academic licence:

> Permission is granted, without written agreement and
> without license or royalty fees, to use, copy, modify,
> and distribute this database and its documentation for
> any purpose, provided that the copyright notice in its
> entirity appear in all copies and the Image and Vision
> Computing Laboratory (IVC) at the University of
> Waterloo is acknowledged in any publication using the
> database.

This fork ships the adapter and the schema in tree, but
**never** the raw clips, the per-clip MOS values, or any
derived feature cache. Only the trained
`nr_metric_v1_*.onnx` weights ship, with the IVC
attribution travelling alongside.

Citation: Li, Z., Duanmu, Z., Liu, W., Wang, Z., "AVC,
HEVC, VP9, AVS2 or AV1? — A Comparative Study of
State-of-the-art Video Encoders on 4K Videos," ICIAR
2019.

## Related

- [ADR-0369: Waterloo IVC 4K-VQA corpus ingestion](../adr/0369-waterloo-ivc-4k-corpus-ingestion.md).
- [Research-0091: Waterloo IVC 4K-VQA corpus feasibility](../research/0091-waterloo-ivc-4k-corpus-feasibility.md).
- ADR-0333 (LSVQ) — same adapter shape; this Waterloo
  IVC adapter is a near-mirror modulo dataset specifics
  (manifest shape + MOS scale).
- [ADR-0325 Phase 2](../adr/0325-konvid-150k-phase2.md) (KonViD-150k) — same
  schema; same resumable-download contract.
- [ADR-0310](../adr/0310-bvi-dvc-corpus-ingestion.md)
  (BVI-DVC) — first second-shard ingestion ADR; sets the
  local-only-corpus / redistributable-derivatives
  precedent.
