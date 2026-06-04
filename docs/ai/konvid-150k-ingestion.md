# KonViD-150k corpus ingestion → MOS-corpus JSONL

The KonViD-150k dataset (Götz-Hahn et al., IEEE Access 2021 / ICIP 2019)
is a ~150 000-clip user-generated-video corpus with crowdsourced
subjective Mean Opinion Scores. The VMAFx fork uses it as **Phase 2**
of the ADR-0325 KonViD ingestion plan: the full-scale follow-on to the
1 200-clip Phase 1 predecessor (see
[`konvid-1k-ingestion.md`](konvid-1k-ingestion.md)). At ~120–200 GB of
working-set disk and hours-to-days of download wall-clock, this path
needs three things the Phase 1 path does not — resumable downloads,
attrition tolerance, and an `"ugc-mixed"` encoder slot — all detailed
below.

## Corpus availability (status 2026-05-15)

The corpus is **materialized locally** at `.workingdir2/konvid-150k/`
(gitignored, ~179 GB). Inventory:

| Artefact | Size | Purpose |
| --- | --- | --- |
| `clips/` | ~150 GB | 307 682 extracted MP4 files |
| `k150ka_scores.csv` | 4.9 MB | k150k-A score-drop |
| `k150kb_scores.csv` | 59 KB | k150k-B score-drop |
| `k150ka_votes.csv` | 94 MB | per-vote raw data (A) |
| `k150kb_votes.csv` | 28 MB | per-vote raw data (B) |
| `konvid_150k.jsonl` | 64 MB | corpus JSONL (Phase 2 adapter output) |
| `manifest.csv` | 4.9 MB | corpus manifest |
| `k150ka_extracted/` + `k150kb_extracted/` | ~8 GB | extracted-frame fixtures |

[ADR-0325](../adr/0325-konvid-150k-corpus-ingestion.md) flipped to
`Accepted` on 2026-05-15 once availability was confirmed. Phase 3
(real-corpus MOS head training) is the next gate; tracked under
[ADR-0336](../adr/0336-konvid-mos-head-v1.md). `train_konvid_mos_head.py`
is unblocked and queued behind the in-flight CHUG feature extraction's
GPU usage; once the GPU frees up, the production-flip gate (`PLCC ≥ 0.85`
mean, `SROCC ≥ 0.82`, `RMSE ≤ 0.45`) gets run.

See [ADR-0325](../adr/0325-konvid-150k-corpus-ingestion.md) for the
two-phase decision and
[Research-0086](../research/0086-konvid-150k-corpus-feasibility.md)
for the feasibility analysis.

## 1. Dataset overview

KonViD-150k ships **distorted-only** content — every clip is a
user-uploaded YouTube/Vimeo encode and every clip carries one
subjective MOS aggregated from ≥ 5 crowdworker ratings on a 1–5 scale.
There is no separate raw reference; each clip is its own opinion datum.
Clips are mostly 540p / 720p / 1080p, ~5–10 s long, h264 / vp9.

Citation:

> Franz Götz-Hahn, Vlad Hosu, Hanhe Lin, Dietmar Saupe. *KonVid-150k:
> A Dataset for No-Reference Video Quality Assessment of Videos
> in-the-Wild.* IEEE Access 2021 (companion to the ICIP 2019 release).

## 2. Where to download

KonViD-150k is distributed by the University of Konstanz / MMSP group.
Depending on the drop, operators may have either a URL manifest CSV
or the canonical split score CSVs plus already-extracted MP4s. **The
fork does not redistribute the corpus, the per-clip MOS values, or any
derived per-clip statistics** (license is research-only — see
ADR-0325 §License).

URL-manifest layout:

```text
.workingdir2/konvid-150k/
  ├── manifest.csv                   # operator drops this
  ├── .download-progress.json        # written by this script (resumable state)
  ├── clips/                         # populated by this script
  │     ├── 0001.mp4
  │     ├── 0002.mp4
  │     └── ...
  └── konvid_150k.jsonl              # written by this script (output)
```

Split score-drop layout:

```text
.workingdir2/konvid-150k/
  ├── k150ka_scores.csv              # video_name,video_score
  ├── k150ka_extracted/              # K150K-A MP4 clips
  ├── k150kb_scores.csv              # video_name,mos,video_score
  ├── k150kb_extracted/              # K150K-B MP4 clips
  └── konvid_150k.jsonl              # written by this script (output)
```

All of these paths are gitignored (`.workingdir2/` is the fork's
standard research-data drop, see CLAUDE.md §5). With a URL manifest,
the script downloads on demand into `clips/`. With the split score
layout, the script reads the local extracted MP4s directly; it does
not try to reconstruct source URLs.

Dataset URL: <https://database.mmsp-kn.de/konvid-150k-vqa-database.html>

## 3. Pipeline

The Phase 2 ingestion is a per-row download → ffprobe → JSONL emit
loop:

```text
   .workingdir2/konvid-150k/manifest.csv
            │
            │  ai/scripts/konvid_150k_to_corpus_jsonl.py
            │      (per row: curl → ffprobe → MOS join)
            │      (writes .download-progress.json after each clip)
            ▼
   .workingdir2/konvid-150k/konvid_150k.jsonl
            │
            │  (Phase 3 — out of scope here, ADR-0325 §Phase 3)
            ▼
   ensemble-training-kit MOS-head trainer
```

For each manifest row the script:

1. Checks the resumable-download progress JSON. If the clip is already
   marked `done` and present on disk, skip the network call.
2. Otherwise `curl --location --fail` the URL into a sibling `.part`
   file and rename atomically on success. Failures are logged with a
   reason and recorded as `failed` in the progress JSON.
3. Run ffprobe on the downloaded MP4 to recover
   `(width, height, fps, duration, pix_fmt, codec_name)`.
4. Join with the manifest CSV's MOS / SD / rating-count columns and
   emit one JSONL row.

Heavy feature extraction (libvmaf, GPU-cached parquet) does **not**
run here — Phase 2 stays a pure metadata-and-MOS join. That keeps the
working set bounded by the clip corpus itself, not by intermediate
parquet shards.

## 4. Run command

After dropping either the URL manifest or the split score/extracted
layout under `.workingdir2/konvid-150k/`:

```bash
python ai/scripts/konvid_150k_to_corpus_jsonl.py
```

Default output path is `.workingdir2/konvid-150k/konvid_150k.jsonl`.
Override with `--output`. The adapter writes `<output>.manifest.json` by
default; pass `--manifest-out PATH` when the replay sidecar should live in
a dated experiment bundle. Override the input root with `--konvid-dir`.
Passing `--manifest-csv` is strict: if that explicit file is missing,
the script fails instead of falling back to split CSV discovery. This
catches typoed manifest paths. Override the curl / ffprobe binaries
with `--curl-bin` / `--ffprobe-bin` (also picked up from `$CURL_BIN` /
`$FFPROBE_BIN`). Override the resumable-state path with
`--progress-path`. The replay manifest records those source paths, row
caps, attrition counters, and ADR-0661 `run_provenance`.

The summary line lands on stderr on completion:

```text
[konvid-150k-jsonl] wrote N rows, skipped M (download-failed),
                   K (broken-clip), L dedups -> <path>
```

## 5. Resumable downloads

The single largest difference from Phase 1: a full pass takes
hours-to-days, so `Ctrl-C` + re-run **must** pick up where the prior
run left off without re-downloading. State is recorded in
`.workingdir2/konvid-150k/.download-progress.json`, written
atomically (tempfile + rename) at most every 50 clips and once at the
end of every run.

State shape:

```json
{
  "0001.mp4": {"state": "done"},
  "0002.mp4": {"state": "failed", "reason": "curl-rc=22: HTTP 404 Not Found"}
}
```

States and their re-run semantics:

| State     | On disk | Re-run behaviour                     |
| --------- | ------- | ------------------------------------ |
| (no key)  | no      | Attempt download                     |
| `done`    | yes     | Skip network call; probe + emit      |
| `done`    | no      | Re-download (file went missing)      |
| `failed`  | n/a     | **Skip; do not retry**               |

The `failed` state is non-retriable on purpose — repeatedly hammering
a 404 / takedown wastes bandwidth and slows convergence. To force a
retry of every previously-failed clip, delete `.download-progress.json`
before re-running. To selectively retry, hand-edit the JSON and remove
the failed entries.

## 6. Attrition handling (~5–8 % expected)

KonViD-150k clips are pulled per-URL from YouTube/Vimeo, so a
non-trivial fraction will fail to download on any given pass —
takedowns, region blocks, channel deletions, and Vimeo URL drift
combine to a 5–8 % attrition rate per recent papers (Götz-Hahn 2021).
The script tolerates this:

- Each download failure is logged at WARNING level with the curl
  reason.
- The summary breaks failures into `(download-failed)` (the byte
  stream never arrived) vs `(broken-clip)` (downloaded but ffprobe
  rejected) so the operator can tell content attrition from on-disk
  corruption at a glance.
- An advisory WARNING fires when the download-failure rate exceeds
  `--attrition-warn-threshold` (default `0.10`, i.e. 10 %, slightly
  above the cited 5–8 % to absorb normal jitter). The run still
  completes — the threshold is informational, not fatal.

A typical clean run logs roughly:

```text
[konvid-150k-jsonl] wrote 142037 rows, skipped 7891 (download-failed),
                   72 (broken-clip), 0 dedups -> .../konvid_150k.jsonl
```

## 7. Output schema

One JSON object per line:

```json
{
  "src": "0001.mp4",
  "src_sha256": "ab12...",
  "src_size_bytes": 4321987,
  "width": 960,
  "height": 540,
  "framerate": 30.0,
  "duration_s": 8.0,
  "pix_fmt": "yuv420p",
  "encoder_upstream": "h264",
  "mos": 3.42,
  "mos_std_dev": 0.51,
  "n_ratings": 7,
  "corpus": "konvid-150k",
  "corpus_version": "konvid-150k-2019",
  "ingested_at_utc": "2026-05-08T12:00:00+00:00"
}
```

The schema is **identical** to Phase 1 modulo `corpus = "konvid-150k"`
and the default `corpus_version` (`konvid-150k-2019`, the ICIP release
year, overridable via `--corpus-version`). Phase 1 and Phase 2 JSONLs
can be concatenated and consumed by a single trainer; the `corpus`
column carries the shard identity for downstream analysis.

The schema is **disjoint** from the existing vmaf-tune Phase A
`CORPUS_ROW_KEYS` row (no `vmaf_score`, `encoder`, `preset`, `crf`).
The two corpora are merged at the trainer level in Phase 3 — not at
the JSONL level — because their natural keys differ. See the Phase 1
docs page for the full rationale.

## 8. Encoder vocabulary preparation

Per ADR-0325 §Decision the YouTube/Vimeo encoder distribution
collapses to a single new `"ugc-mixed"` slot in `ENCODER_VOCAB` v4.

This script does **not** modify `ENCODER_VOCAB` itself — that's a
separate PR per ADR-0325 §Phase 2. The script simply records the
upstream codec name ffprobe reports (`h264` / `vp9` / `av1` / etc.)
in the `encoder_upstream` JSONL field. The trainer is responsible for
collapsing every `encoder_upstream` value from a `konvid-*` corpus row
into the `"ugc-mixed"` slot at consumption time.

## 9. Refusal: KonViD-1k mis-mount

If the operator points the script at a `manifest.csv` that actually
holds the KonViD-1k attribute CSV (~1 200 rows), the script aborts
with a hint pointing at `konvid_1k_to_corpus_jsonl.py` (Phase 1). The
threshold is **5 000 rows** — the actual KonViD-150k size is
~150 000, so the gap absorbs pre-release subsets while still catching
the inverse-mount case. This mirrors the Phase 1 1500-row ceiling in
the inverse direction.

## 10. Reproducibility and CI

Every step is deterministic given the same input archive. The
`src_sha256` field is a chunked SHA-256 of the clip bytes (1 MiB
chunks; same shape `vmaftune.corpus.py` already emits) so re-runs
across machines produce identical hashes for identical clips.

CI cannot retrain end-to-end (the corpus is non-redistributable). The
adapter is exercised by
[`ai/tests/test_konvid_150k.py`](../../ai/tests/test_konvid_150k.py),
which mocks both ffprobe **and** curl via the script's injectable
`runner` seam and stands up a temporary
`.workingdir2/konvid-150k/`-shaped tree on disk. The tests run in well
under one second and require neither curl, ffprobe, nor the corpus.

## 11. Operational notes

- **Resume is silent and lossless.** A `Ctrl-C` mid-download is safe:
  the in-flight `*.part` file is deleted on next-run start, the
  progress JSON was last flushed at most 50 clips ago, and the JSONL
  uses `src_sha256`-keyed dedup so a re-run cannot double-emit.
- **Atomic state file.** `.download-progress.json` is written via
  tempfile + `os.replace` — the operator never observes a half-written
  state file even on `kill -9`.
- **Failed-clip retry policy.** A `failed` entry stays `failed` until
  the operator deletes it (or the entire JSON). Rationale: most
  `failed` reasons are content takedowns that will never resolve.
- **CSV column-name aliases.** The manifest CSV has shipped under
  varying spellings (`MOS` vs `mos`, `SD` vs `mos_std`, `n` vs
  `num_ratings`, `file_name` vs `video_name`, `url` vs `download_url`).
  The adapter accepts every spelling that has shipped to date; if a
  new release adds an alias, edit `_CSV_*_KEYS` at the top of the
  script.
- **Split score-drop aliases.** `k150ka_scores.csv` commonly carries
  `video_name,video_score`; `k150kb_scores.csv` commonly carries
  `video_name,mos,video_score`. Both are accepted. Split rows have no
  URL, per-row standard deviation, or rating count in the score CSVs.
  The internal download field is empty, and the emitted JSONL records
  `mos_std_dev = 0.0` and `n_ratings = 0`.
- **License posture.** Per ADR-0325 §License, neither the clips, the
  per-clip MOS values, nor the JSONL itself ship in the repo. Only the
  ingestion script, this docs page, and the schema definition are
  in-tree. Trained model weights derived from the corpus are
  redistributable; per-clip data is not.

## 12. Next phase

- **Phase 3.** Train a sibling MOS-head ONNX via the existing
  ensemble-training-kit harness (ADR-0324). Hold out a 10 % fold for
  production-flip gating per the ADR-0303 protocol; concurrently use
  the held-out fold to measure how well the existing VMAF predictor
  correlates with subjective MOS on UGC content.
