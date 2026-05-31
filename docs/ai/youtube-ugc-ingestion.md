<!-- markdownlint-disable MD060 -->
# YouTube UGC -> MOS-corpus JSONL ingestion

The fork's `nr_metric_v1` tiny no-reference VQA model is trained
on a union of MOS-corpus shards. This page documents the
YouTube UGC shard ingestion adapter
(`ai/scripts/youtube_ugc_to_corpus_jsonl.py`, ADR-0368).

## What YouTube UGC is

The Google YouTube UGC dataset (Wang, Inguva, Adsumilli; MMSP
2019) is the field's canonical large-scale UGC corpus. ~1500
original community-uploaded clips spanning gaming, vlogs,
lyric-videos, sports, HDR, and animation, with crowd MOS values
on the same 1.0-5.0 Likert scale as LSVQ / KonViD.

- **Public bucket**:
  <https://storage.googleapis.com/ugc-dataset/>
  (CC-BY, no sign-up, no request form).
- **Original-video listing CSV**:
  <https://storage.googleapis.com/ugc-dataset/original_videos.csv>.
- **Attribution / license**:
  <https://storage.googleapis.com/ugc-dataset/ATTRIBUTION>.

## When to run the adapter

Run this adapter when you want to (re-)build the YouTube UGC
MOS-corpus JSONL shard the trainer consumes alongside LSVQ
(ADR-0333), KonViD-150k (ADR-0325 Phase 2), and BVI-DVC
(ADR-0310).

## Prerequisites

1. ~2 TB free disk under `.workingdir2/youtube-ugc/` for the
   whole-corpus run, or ~10 GB for the laptop-class default.
2. `curl` and `ffprobe` on `$PATH`.
3. The manifest CSV from the bucket dropped at
   `.workingdir2/youtube-ugc/manifest.csv` (or pass
   `--manifest-csv` to point elsewhere).

## Quick start (laptop-class subset)

```bash
# Drop original_videos.csv at the default location, then:
python ai/scripts/youtube_ugc_to_corpus_jsonl.py
# -> reads .workingdir2/youtube-ugc/manifest.csv
# -> caps at the first --max-rows=300 clips (default)
# -> downloads each via curl into .workingdir2/youtube-ugc/clips/
# -> writes .workingdir2/youtube-ugc/youtube-ugc.jsonl
```

If the manifest CSV does not carry a URL column (the canonical
`original_videos.csv` does not), the adapter synthesises the
download URL from `--bucket-prefix` (default
`https://storage.googleapis.com/ugc-dataset/original_videos/`).

The separate VP9 compressed subset helper
`ai/scripts/fetch_youtube_ugc_subset.py` keeps its existing `--manifest`
stem-to-files JSON for downstream consumers and writes a run sidecar at
`<manifest>.run-manifest.json` by default. Pass `--run-manifest-out PATH` for a
dated experiment bundle. The run sidecar records the GCS listing URL, smallest
complete-4tuple selection policy, selected file sizes, output directory, and
ADR-0661 `run_provenance`.

## Whole-corpus ingestion

```bash
python ai/scripts/youtube_ugc_to_corpus_jsonl.py --full
```

This disables the `--max-rows` cap. Working set is ~2 TB on the
canonical original-videos manifest. The run is resumable:
`Ctrl-C` mid-download is safe, and re-running picks up from
`.workingdir2/youtube-ugc/.download-progress.json` (atomic
tempfile-rename writes).

## Output schema

One JSON object per line in
`.workingdir2/youtube-ugc/youtube-ugc.jsonl`:

```jsonc
{
  "src":               "Gaming_720P-25aa_orig.mp4",
  "src_sha256":        "<hex>",
  "src_size_bytes":    1234567,
  "width":             1280,
  "height":            720,
  "framerate":         30.0,
  "duration_s":        20.0,
  "pix_fmt":           "yuv420p",
  "encoder_upstream":  "h264",
  "mos":               3.42,
  "mos_std_dev":       0.51,
  "n_ratings":         48,
  "corpus":            "youtube-ugc",
  "corpus_version":    "ugc-2019-orig",
  "ingested_at_utc":   "2026-05-08T10:00:00+00:00"
}
```

The schema is byte-identical to the LSVQ / KonViD-150k
adapters' modulo the `corpus` and `corpus_version` literals. MOS
is recorded verbatim on the dataset's native 1.0-5.0 scale (no
rescaling at ingest time); the trainer-side data loader is
responsible for any per-corpus normalisation.

## Per-clip scoring methodology

Two distinct subjective releases sit under the same dataset
umbrella:

1. **2019 originals release** (default,
   `--corpus-version=ugc-2019-orig`) — per-clip crowd MOS on the
   1.0-5.0 Likert scale across 1380 of the ~1500 originals.
   Pass-through identical to LSVQ.
2. **2020 transcoded follow-up**
   (`--corpus-version=ugc-2020-transcoded-mean`) — per-bitrate
   crowd ratings on transcoded outputs at four rate points
   (`orig` / `cbr` / `vod` / `vodlb`). Operators wanting these
   ratings pre-aggregate them into a one-row-per-`orig` CSV with
   the per-clip mean across the four levels; the adapter records
   the mean verbatim.

The adapter records whatever the manifest's MOS column contains,
without rescaling. Documenting the methodology behind the
manifest's MOS column is the operator's responsibility (it
propagates through the row's `corpus_version` literal).

## Manifest CSV column-name aliases

YouTube UGC manifest CSVs ship with slightly different header
spellings across the 2019 MMSP release, the 2020 transcoded
follow-up, and DOVER / FAST-VQA redistributions. The adapter
accepts every observed alias:

| Logical column | Recognised header spellings |
|---|---|
| filename | `vid`, `name`, `video_name`, `filename`, `file_name` |
| URL (optional) | `url`, `download_url`, `video_url` |
| MOS | `mos`, `MOS`, `mos_score`, `dmos`, `DMOS` |
| MOS std-dev | `sd`, `SD`, `mos_std`, `mos_std_dev`, `sd_mos`, `SD_MOS` |
| rating count | `n`, `ratings`, `num_ratings`, `n_ratings` |

Bare-stem filenames (e.g. `Gaming_720P-25aa_orig`) are normalised
to `Gaming_720P-25aa_orig.mp4` by appending `--clip-suffix`
(default `.mp4`).

## Operator flags

```text
--ugc-dir PATH                Working dir (default: .workingdir2/youtube-ugc/)
--manifest-csv PATH           Path to manifest CSV (default: <dir>/manifest.csv)
--progress-path PATH          Resumable state file
                              (default: <dir>/.download-progress.json)
--clips-subdir NAME           Subdir for clips (default: clips)
--clip-suffix EXT             Default file suffix (default: .mp4)
--bucket-prefix URL           Public bucket URL prefix used to synthesise
                              download URLs when manifest lacks a `url` column
                              (default: original_videos/ on ugc-dataset bucket)
--output PATH                 Output JSONL (default: <dir>/youtube-ugc.jsonl)
--manifest-out PATH           Replay manifest JSON sidecar
                              (default: <output>.manifest.json)
--ffprobe-bin BIN             ffprobe binary (default: $FFPROBE_BIN or ffprobe)
--curl-bin BIN                curl binary (default: $CURL_BIN or curl)
--corpus-version STR          Dataset version (default: ugc-2019-orig;
                              pass ugc-2020-transcoded-mean for the
                              transcoded-mean variant)
--attrition-warn-threshold F  Advisory failure-rate floor (default: 0.10)
--download-timeout-s N        Per-clip curl --max-time (default: 300;
                              UGC clips are large)
--max-rows N                  Row cap (default: 300; laptop-class subset)
--full                        Disable --max-rows cap; ingest whole CSV
--log-level LEVEL             DEBUG / INFO / WARNING / ERROR
```

The replay manifest records the UGC working directory, manifest/progress paths,
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
  exceeds `--attrition-warn-threshold` (default 10%), an
  advisory WARNING is logged. The run still completes.

## License & redistribution

YouTube UGC is Creative Commons Attribution. This fork ships
the adapter and the schema in tree, but **never** the raw clips,
the per-clip MOS values, or any derived feature cache. Only the
trained `nr_metric_v1_*.onnx` weights ship, with CC-BY
attribution travelling alongside.

## Related

- [ADR-0368: YouTube UGC corpus ingestion](../adr/0368-youtube-ugc-corpus-ingestion.md).
- [Research-0091: YouTube UGC corpus feasibility](../research/0091-youtube-ugc-corpus-feasibility.md).
- [ADR-0333](../adr/0333-lsvq-corpus-ingestion.md) (LSVQ) —
  same adapter shape; this YouTube UGC adapter is a near-mirror
  modulo dataset specifics + the synthesised-bucket-URL path.
- ADR-0325 Phase 2 (KonViD-150k) — schema co-author.
- [ADR-0310](../adr/0310-bvi-dvc-corpus-ingestion.md) (BVI-DVC) —
  the first second-shard ingestion ADR; sets the
  local-only-corpus / redistributable-derivatives precedent.
