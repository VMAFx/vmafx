# KonViD-1k corpus ingestion → MOS-corpus JSONL

The KonViD-1k dataset (Hosu et al., QoMEX 2017) is a 1,200-clip
user-generated-video corpus with crowdsourced subjective Mean Opinion
Scores. The VMAFx fork uses it as **Phase 1** of the ADR-0325
KonViD-150k ingestion plan: a small, fast-to-iterate predecessor that
validates the JSONL conversion shape before scaling to the full ~150 k
corpus in Phase 2.

See [ADR-0325](../adr/0325-konvid-150k-corpus-ingestion.md) for the
two-phase decision and [Research-0086](../research/0086-konvid-150k-corpus-feasibility.md)
for the feasibility analysis.

## 1. Dataset overview

KonViD-1k ships **distorted-only** content — every clip is a
user-uploaded YouTube/Flickr encode, and every clip carries one
subjective MOS aggregated from ≥ 50 crowdworker ratings on a 1–5
scale. There is no separate raw reference; each clip is its own
opinion datum. Clips are mostly 540p, ~8 s long, h264 / vp9.

Citation:

> Vlad Hosu, Franz Hahn, Mohsen Jenadeleh, Hanhe Lin, Hui Men, Tamás
> Szirányi, Shujun Li, Dietmar Saupe. *The Konstanz natural video
> database (KoNViD-1k).* QoMEX 2017.

## 2. Where to download

KonViD-1k is distributed by the University of Konstanz / MMSP group as
two zip files (videos + metadata). **The fork does not redistribute the
corpus, the per-clip MOS values, or any derived per-clip statistics**
(license is research-only — see ADR-0325 §License). Obtain it from
the upstream source and place it locally:

```text
.workingdir2/konvid-1k/
  ├── KoNViD_1k_videos/
  │     ├── 1.mp4
  │     ├── 2.mp4
  │     └── ... (1200 clips)
  └── KoNViD_1k_metadata/
        └── KoNViD_1k_attributes.csv
```

Both paths are gitignored (`.workingdir2/` is the fork's standard
research-data drop, see CLAUDE.md §5). The companion downloader
[`ai/scripts/fetch_konvid_1k.py`](../../ai/scripts/fetch_konvid_1k.py)
will fetch + extract everything to a similarly-shaped location under
`$VMAF_DATA_ROOT/konvid-1k/`; the ingestion script accepts either
layout via `--konvid-dir`.
The fetcher writes `<root>/fetch_manifest.json` by default and accepts
`--manifest-out PATH`. That sidecar records the archive URLs, observed archive
sizes, extracted-directory status, `--keep-zips`, and ADR-0661
`run_provenance` before the JSONL adapter consumes the local root.

Dataset URL: <https://database.mmsp-kn.de/konvid-1k-database.html>

## 3. Pipeline

The Phase 1 ingestion is a single transform:

```text
   .workingdir2/konvid-1k/
            │
            │  ai/scripts/konvid_1k_to_corpus_jsonl.py
            │      (ffprobe per clip + CSV MOS join)
            ▼
   .workingdir2/konvid-1k/konvid_1k.jsonl
            │
            │  (Phase 3 — out of scope here, ADR-0325 §Phase 3)
            ▼
   ensemble-training-kit MOS-head trainer
```

The script runs ffprobe once per clip, joins with the attribute
CSV's MOS / SD / rating-count columns, and emits one JSONL row per
clip. Heavy feature extraction (libvmaf, GPU-cached parquet) does **not**
run here — Phase 1 is a pure metadata-and-MOS join. That keeps the
working set small (~50 MB JSONL on top of the 5 GB clip corpus) and
re-runs idempotent.

## 4. Run command

After dropping the extracted KonViD-1k under `.workingdir2/konvid-1k/`:

```bash
python ai/scripts/konvid_1k_to_corpus_jsonl.py
```

Default output path is `.workingdir2/konvid-1k/konvid_1k.jsonl`. Override
with `--output`. Override the input layout with `--konvid-dir`. Override
the ffprobe binary with `--ffprobe-bin` (also picked up from
`$FFPROBE_BIN`).

The adapter writes `<output>.manifest.json` by default and accepts
`--manifest-out PATH` when a run bundle needs a separate sidecar location.
The manifest records the KonViD root, effective corpus version, row counters,
and ADR-0661 `run_provenance`.

The summary line lands on stderr on completion:

```text
[konvid-1k-jsonl] wrote N rows, skipped M (broken), K dedups -> <path>
```

Re-running against an existing output is idempotent: clips already
present (keyed by `src_sha256`) are dedup'd; new clips are appended.
Existing rows are never rewritten.

## 5. Output schema

One JSON object per line:

```json
{
  "src": "1234567.mp4",
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
  "n_ratings": 64,
  "corpus": "konvid-1k",
  "corpus_version": "konvid-1k-2017",
  "ingested_at_utc": "2026-05-08T12:00:00+00:00"
}
```

The schema is **disjoint** from the existing vmaf-tune Phase A
`CORPUS_ROW_KEYS` row (no `vmaf_score`, `encoder`, `preset`, `crf`).
The two corpora are merged at the trainer level in Phase 3 — not at
the JSONL level — because their natural keys differ: vmaf-tune rows
key on `(src_sha256, encoder, preset, crf)` (synthetic encodes of a
known reference), while KonViD rows key on `src_sha256` alone (the
clip *is* the artefact, and it carries a human MOS instead of an
algorithmic VMAF score).

The `corpus_version` field defaults to `"konvid-1k-2017"` (the QoMEX
release year) and is overridable via `--corpus-version` for downstream
shards (e.g. a re-rated 2019 metadata refresh).

## 6. Refusal: KonViD-150k mis-mount

If the operator points the script at a `KoNViD_1k_metadata/` directory
that actually holds the KonViD-150k attribute CSV (~150 000 rows), the
script aborts with a hint pointing at `konvid_150k_to_corpus_jsonl.py`
(Phase 2; not yet shipped — see ADR-0325 §Phase 2). The threshold is
**1500 rows** — the actual KonViD-1k size is exactly 1200, so the
gap absorbs minor index-row variations without false positives. This
guards against silently ingesting a 100 × larger corpus through a
1k-shaped pipeline; the geometry probe alone would take days at that
scale and the disk impact (200+ GB) would surprise the operator.

## 7. Reproducibility and CI

Every step is deterministic given the same input archive. The
`src_sha256` field is a chunked SHA-256 of the clip bytes (1 MiB
chunks; same shape `vmaftune.corpus.py` already emits) so re-runs
across machines produce identical hashes for identical clips.

CI cannot retrain end-to-end (the corpus is non-redistributable). The
adapter is exercised by [`ai/tests/test_konvid_1k.py`](../../ai/tests/test_konvid_1k.py),
which mocks ffprobe via a synthesised JSON payload and stands up a
temporary `.workingdir2/konvid-1k/`-shaped tree on disk. The tests run
in well under one second and require neither ffprobe nor the corpus.

## 8. Operational notes

- **Broken clips are skipped, not fatal.** If ffprobe returns a
  non-zero exit code, fails to parse JSON, or reports zero streams /
  zero geometry, the clip is logged as `skipped (broken)` and the run
  continues. The summary line reports the count.
- **CSV column-name aliases.** The attribute CSV uses different column
  names across the 2017 / 2019 dataset releases (`MOS` vs `mos`, `SD`
  vs `mos_std`, `n` vs `num_ratings`, `file_name` vs `video_name`).
  The adapter accepts every spelling that has shipped to date; if a
  release adds a new alias, edit `_CSV_*_KEYS` at the top of the
  script.
- **License posture.** Per ADR-0325 §License, neither the clips, the
  per-clip MOS values, nor the JSONL itself ship in the repo. Only the
  ingestion script, this docs page, and the schema definition are
  in-tree. Trained model weights derived from the corpus are
  redistributable; per-clip data is not.

## 9. Next phases

- **Phase 1.5 (optional).** Drop the same script-shape against
  YouTube-UGC (Google's 1.5k-clip MOS+VMAF set) for a cross-corpus
  sanity check.
- **Phase 2.** Scale to KonViD-150k via
  `ai/scripts/konvid_150k_to_corpus_jsonl.py` (not yet shipped — see
  ADR-0325 §Phase 2). Adds resumable downloads, ~5–8 % attrition
  tolerance, and an `"ugc-mixed"` ENCODER_VOCAB slot.
- **Phase 3.** Train a sibling MOS-head ONNX via the existing
  ensemble-training-kit harness (ADR-0324). Held-out fold gates
  production-flip via the ADR-0303 protocol.
