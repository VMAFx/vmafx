<!-- markdownlint-disable MD060 -->
# LSVQ → MOS-corpus JSONL ingestion

The fork's `nr_metric_v1` tiny no-reference VQA model is trained
on a union of MOS-corpus shards. This page documents the LSVQ
shard ingestion adapter
(`ai/scripts/lsvq_to_corpus_jsonl.py`, ADR-0367).

## What LSVQ is

LSVQ — LIVE Large-Scale Social Video Quality (Ying, Mandal,
Ghadiyaram, Bovik; ICCV 2021) — is the canonical large-scale
no-reference VQA training corpus: ~39 000 user-generated videos
with ~5.5 M individual subjective ratings collapsed into per-clip
MOS values on a 1.0–5.0 Likert scale.

- **Canonical splits**: `LSVQ_whole_train` (~28 056 clips),
  `LSVQ_test` (~7 400 clips, mixed-resolution),
  `LSVQ_test_1080p` (~3 600 1080p clips).
- **Hugging Face mirror**:
  <https://huggingface.co/datasets/teowu/LSVQ-videos>
  (CC-BY-4.0).
- **Author drop**: <https://github.com/baidut/PatchVQ>.

## When to run the adapter

Run this adapter when you want to (re-)build the
LSVQ MOS-corpus JSONL shard the trainer consumes
alongside KonViD-150k and BVI-DVC.

## Prerequisites

1. ~500 GB free disk under `.workingdir2/lsvq/` for the
   whole-corpus run, or ~5 GB for the laptop-class default.
2. `curl` and `ffprobe` on `$PATH`.
3. The split CSV from Hugging Face dropped at
   `.workingdir2/lsvq/manifest.csv` (or pass `--manifest-csv`
   to point elsewhere).

## Quick start (laptop-class subset)

```bash
# Drop the LSVQ_whole_train CSV at the default location, then:
python ai/scripts/lsvq_to_corpus_jsonl.py
# → reads .workingdir2/lsvq/manifest.csv
# → caps at the first --max-rows=500 clips (default)
# → downloads each via curl into .workingdir2/lsvq/clips/
# → writes .workingdir2/lsvq/lsvq.jsonl
```

## Whole-corpus ingestion

```bash
python ai/scripts/lsvq_to_corpus_jsonl.py --full
```

This disables the `--max-rows` cap. Working set is ~500 GB on
`LSVQ_whole_train`. The run is resumable: `Ctrl-C` mid-download
is safe, and re-running picks up from
`.workingdir2/lsvq/.download-progress.json` (atomic
tempfile-rename writes).

## Output schema

One JSON object per line in `.workingdir2/lsvq/lsvq.jsonl`:

```jsonc
{
  "src":               "0001.mp4",
  "src_sha256":        "<hex>",
  "src_size_bytes":    1234567,
  "width":             1920,
  "height":            1080,
  "framerate":         30.0,
  "duration_s":        8.0,
  "pix_fmt":           "yuv420p",
  "encoder_upstream":  "h264",
  "mos":               3.42,
  "mos_std_dev":       0.51,
  "n_ratings":         48,
  "corpus":            "lsvq",
  "corpus_version":    "lsvq-2021",
  "ingested_at_utc":   "2026-05-08T10:00:00+00:00"
}
```

The schema is byte-identical to the KonViD-150k Phase 2 adapter's
modulo the `corpus` and `corpus_version` literals. MOS is
recorded verbatim on the LSVQ-native 1.0–5.0 scale (no rescaling
at ingest time); the trainer-side data loader is responsible for
any per-corpus normalisation.

## Manifest CSV column-name aliases

LSVQ split CSVs ship with slightly different header spellings
across the ICCV-2021 author drop, the Hugging Face mirror, and
DOVER / FAST-VQA redistributions. The adapter accepts every
observed alias:

| Logical column | Recognised header spellings |
|---|---|
| filename | `name`, `video_name`, `filename`, `file_name` |
| URL (optional) | `url`, `download_url`, `video_url` |
| MOS | `mos`, `MOS`, `mos_score` |
| MOS std-dev | `sd`, `SD`, `mos_std`, `mos_std_dev`, `SD_MOS` |
| rating count | `n`, `ratings`, `num_ratings`, `n_ratings` |

Bare-stem filenames (e.g. `0001`) are normalised to
`0001.mp4` by appending `--clip-suffix` (default `.mp4`).

## Operator flags

```text
--lsvq-dir PATH               Working dir (default: .workingdir2/lsvq/)
--manifest-csv PATH           Path to split CSV (default: <dir>/manifest.csv)
--progress-path PATH          Resumable state file
                              (default: <dir>/.download-progress.json)
--clips-subdir NAME           Subdir for clips (default: clips)
--clip-suffix EXT             Default file suffix (default: .mp4)
--output PATH                 Output JSONL (default: <dir>/lsvq.jsonl)
--manifest-out PATH           Replay manifest JSON sidecar
                              (default: <output>.manifest.json)
--ffprobe-bin BIN             ffprobe binary (default: $FFPROBE_BIN or ffprobe)
--curl-bin BIN                curl binary (default: $CURL_BIN or curl)
--corpus-version STR          Dataset version (default: lsvq-2021)
--attrition-warn-threshold F  Advisory failure-rate floor (default: 0.10)
--download-timeout-s N        Per-clip curl --max-time (default: 120)
--max-rows N                  Row cap (default: 500; laptop-class subset)
--full                        Disable --max-rows cap; ingest whole CSV
--log-level LEVEL             DEBUG / INFO / WARNING / ERROR
```

The replay manifest records the LSVQ working directory, manifest/progress paths,
row cap, attrition counters, effective corpus version, and ADR-0661
`run_provenance`.

## Failure handling

- **Download failure** (HTTP 404 / 410, curl spawn failure,
  empty body): logged with the reason, persisted to the progress
  file as `state: "failed"`, run continues. Re-runs honour the
  non-retry contract — to retry, delete the entry from the
  progress file or delete the whole file.
- **ffprobe failure** ("broken-clip"): logged, run continues,
  no row emitted. Distinct from download-failed in the summary
  line.
- **Attrition WARNING**: when the download-failed fraction
  exceeds `--attrition-warn-threshold` (default 10 %), an
  advisory WARNING is logged. The run still completes.

## License & redistribution

LSVQ is CC-BY-4.0. This fork ships the adapter and the schema
in tree, but **never** the raw clips, the per-clip MOS values,
or any derived feature cache. Only the trained
`nr_metric_v1_*.onnx` weights ship, with CC-BY-4.0 attribution
travelling alongside.

## Related

- [ADR-0367: LSVQ corpus ingestion](../adr/0367-lsvq-corpus-ingestion.md).
- [Research-0090: LSVQ corpus feasibility](../research/0090-lsvq-corpus-feasibility.md).
- [Tiny-AI SOTA deep-dive](../research/0086-tiny-ai-sota-deep-dive-2026-05-08.md)
  (LSVQ vs DOVER vs FAST-VQA leaderboard context).
- ADR-0325 Phase 2 (KonViD-150k) — same adapter shape;
  this LSVQ adapter is a near-mirror modulo dataset specifics.
- [ADR-0310](../adr/0310-bvi-dvc-corpus-ingestion.md) (BVI-DVC) —
  the first second-shard ingestion ADR; sets the
  local-only-corpus / redistributable-derivatives precedent.
