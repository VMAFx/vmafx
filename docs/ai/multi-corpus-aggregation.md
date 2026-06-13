<!-- markdownlint-disable MD013 MD060 -->
# Multi-corpus aggregation for the FR-regressor / predictor v2 trainer

The fork ingests several MOS-labelled video-quality corpora — KonViD-1k,
KonViD-150k, LSVQ, Waterloo IVC 4K-VQA, YouTube UGC, and the Netflix
Public drop — each via its own adapter script that emits a corpus-specific
JSONL. The training pipelines (`train_predictor_v2_realcorpus.py` from
PR #487, `train_konvid.py` from PR #491) want **one** unified-scale row
stream so they can learn from every shard simultaneously without the
target-MOS distribution silently warping with the corpus mix.

`ai/scripts/aggregate_corpora.py` is that bridge. See
[ADR-0340](../adr/0340-multi-corpus-aggregation.md) for the decision
record.

## 1. Why a unified-scale step

Different subjective-VQA datasets publish their MOS on different
scales. KonViD / LSVQ / YouTube UGC use a 1–5 ACR Likert; Waterloo
IVC 4K-VQA uses a continuous 0–100 numerical-category scale; the
Netflix Public drop carries `vmaf_v0.6.1` per-frame scores on the
0–100 VMAF axis. A naive concatenation would feed the trainer three
incompatible target distributions and the regression head would learn
the wrong thing. The aggregator picks **0–100 (VMAF-aligned)** as the
single canonical axis and applies a per-corpus *affine* conversion
documented below — affine, never compressed, so the per-corpus
distribution shape is preserved.

## 2. Per-corpus scale conversions

| `corpus_source`    | source scale                       | conversion to 0–100         | citation (access 2026-05-09) |
|--------------------|------------------------------------|-----------------------------|------------------------------|
| `konvid-1k`        | 1.0–5.0 ACR Likert                 | `unified = (mos - 1) * 25`  | Hosu et al., QoMEX 2017 — <http://database.mmsp-kn.de/konvid-1k-database.html> |
| `konvid-150k`      | 1.0–5.0 ACR Likert                 | `unified = (mos - 1) * 25`  | Götz-Hahn et al., IEEE Access 2021 — <https://database.mmsp-kn.de/konvid-150k-vqa-database.html> |
| `lsvq`             | 1.0–5.0 ACR Likert                 | `unified = (mos - 1) * 25`  | Ying et al., CVPR 2021 §4.1 — <https://github.com/baidut/PatchVQ> |
| `youtube-ugc`      | 1.0–5.0 ACR Likert                 | `unified = (mos - 1) * 25`  | Wang et al., MMSP 2019 §3.2 — <https://media.withyoutube.com/> |
| `waterloo-ivc-4k`  | 0–100 continuous (DCR-like)        | identity                    | Cheon & Lee, CVPR-W 2016 §III.B — <https://ece.uwaterloo.ca/~zduanmu/cvpr2016_4kvqa/> |
| `netflix-public`   | VMAF 0–100 (objective proxy)       | identity                    | `core/include/libvmaf/model.h` |

The mapping is a single source of truth in
`ai/scripts/aggregate_corpora.py:SCALE_CONVERSIONS`; the unit tests
under `ai/tests/test_aggregate_corpora.py` exercise it parametrically.

### What happens to questionable inputs

Per the fork's [feedback_no_test_weakening](../../CLAUDE.md) rule, the
aggregator never silently widens the training-target distribution. If
a row's native MOS falls outside its corpus's published range (e.g.
`6.0` on a 1–5 ACR scale), the row is **dropped** and counted under
`dropped_bad_scale`, not clipped. If the row's `corpus` field does
not match any entry in `SCALE_CONVERSIONS`, the row is dropped under
`dropped_unknown_corpus`. The unified JSONL is therefore always a
strict subset of the inputs, with provenance you can verify
row-by-row.

## 3. Cross-corpus dedup

A clip appearing in two corpora (same content fingerprinted by
`src_sha256`) is a duplicate. The aggregator keeps the row whose
`mos_std_dev` is **smaller** — that row carries the tighter
subjective-quality estimate and is the better trainer target.
Tie-breaking is first-seen, which is deterministic given a stable
`--inputs` ordering. A missing or zero `mos_std_dev` is treated as
"unknown uncertainty" and loses to any row that reports a positive
std-dev.

If you'd rather merge by per-encode triple (`(src_sha256, encoder,
preset, crf)`) — the encode-corpus identity used by the FR-regressor
v2 Phase A path — use `ai/scripts/merge_corpora.py` instead. The two
utilities serve different schemas: `merge_corpora.py` for
encode-grid corpora (Netflix + BVI-DVC), `aggregate_corpora.py` for
subjective-MOS corpora.

## 4. Output schema

Each unified row gains four provenance fields on top of the input
schema:

```json
{
  "src": "clip.mp4",
  "src_sha256": "<hex>",
  "width": 1920,
  "height": 1080,
  "framerate": 24.0,
  "duration_s": 5.0,
  "pix_fmt": "yuv420p",
  "encoder_upstream": "h264",
  "mos": 75.0,
  "mos_native": 4.0,
  "mos_native_scale": "1-5-acr",
  "mos_std_dev": 0.5,
  "n_ratings": 30,
  "corpus": "lsvq",
  "corpus_source": "lsvq",
  "corpus_version": "lsvq-2021",
  "ingested_at_utc": "2026-05-08T00:00:00+00:00",
  "aggregated_at_utc": "2026-05-09T00:00:00+00:00"
}
```

The trainer reads `mos` as the regression target; downstream ablation
and per-corpus loss-weighting key on `corpus_source`. `mos_native` +
`mos_native_scale` are kept for round-trip diagnostics.

## 5. Operator workflow

### One-shot

```bash
python ai/scripts/aggregate_corpora.py \
    --inputs .workingdir2/konvid-150k/konvid_150k.jsonl \
             .workingdir2/lsvq/lsvq.jsonl \
             .workingdir2/waterloo-ivc-4k/waterloo_ivc_4k.jsonl \
             .workingdir2/youtube-ugc/youtube_ugc.jsonl \
    --output .workingdir2/aggregated/unified_corpus.jsonl
```

The default sidecar path is
`.workingdir2/aggregated/unified_corpus.manifest.json`. It records
`run_provenance`, input shard hashes, the active scale-conversion table,
corpus-source overrides, and the same counters printed to stderr. Keep this
manifest with the JSONL when using the artifact for later model-card evidence.

Missing input paths are logged as `WARNING` and skipped. The run
fails only when **every** path is absent — an empty unified corpus
is never the operator's intent.

### Discover-then-train (recommended)

```bash
bash ai/scripts/run_aggregated_training.sh
```

The shell wrapper inspects `.workingdir2/` for the conventional
per-corpus JSONL locations, runs the aggregator on whatever is
present, then kicks off `train_predictor_v2_realcorpus.py` (PR #487)
on the unified output. Set `VMAF_AGG_DRY_RUN=1` to skip the trainer
kick-off (useful when validating ingestion in CI).

## 6. Failure modes and recovery

| symptom | cause | fix |
|---------|-------|-----|
| `error: no input JSONL files exist` | every path under `--inputs` is absent | run at least one ingestion adapter (e.g. `python ai/scripts/konvid_150k_to_corpus_jsonl.py`) |
| `error: ...:N: missing required keys` | input JSONL was produced by a stale adapter that pre-dates the `src_sha256` field | re-run the adapter; the aggregator will not silently fill missing keys |
| `WARNING ... unknown corpus label 'foo'; row dropped` | the row's `corpus` field is non-canonical | pass `--corpus-source-override path/to/foo.jsonl=lsvq` (or whichever known label fits) |
| `WARNING ... outside the published [a,b] range; row dropped` | a value-out-of-range row in the input | inspect the row in the source JSONL; do **not** widen the slack in code |

## 7. Testing

```bash
python -m pytest ai/tests/test_aggregate_corpora.py -v
```

The test suite covers per-corpus conversion accuracy, cross-corpus
dedup, partial-corpus runs, missing-input degradation, schema
violations, and unknown-corpus labels. It does not require any
corpus JSONL on disk — every input is synthesised in-memory.

## 8. References

- [ADR-0340: multi-corpus aggregation](../adr/0340-multi-corpus-aggregation.md) — decision record.
- [ADR-0310: BVI-DVC corpus ingestion](../adr/0310-bvi-dvc-corpus-ingestion.md) — sibling encode-corpus merge utility (`merge_corpora.py`).
- [ADR-0669: AI corpus JSONL provenance](../adr/0669-ai-corpus-jsonl-provenance.md) — manifest sidecars for aggregation and merge outputs.
- [ADR-0325: KonViD-150k corpus ingestion](../adr/0325-konvid-150k-corpus-ingestion.md) — Phase 2 KonViD adapter.
- ADR-0333 (LSVQ ingestion, in flight on PR #471).
- ADR-0334 (YouTube UGC + Waterloo IVC ingestion, in flight on PRs #481 / #485).
