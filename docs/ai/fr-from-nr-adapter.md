<!-- markdownlint-disable MD060 -->
# FR-from-NR adapter (KonViD-150k → FR corpus)

The fork's predictor schema (`fr_regressor_v2_ensemble`,
`fr_regressor_v3`) is **full-reference**: every training row carries the
canonical-6 features (`adm2`, `vif_scale0..3`, `motion2`) computed
between a reference YUV and a re-encoded distortion. The
KonViD-150k shard ([ADR-0325](../adr/0325-konvid-150k-corpus-ingestion.md))
is **no-reference**: each row is a YouTube-UGC distorted MP4 plus a
crowdworker MOS, no clean reference video.

The FR-from-NR adapter ([ADR-0346](../adr/0346-fr-features-from-nr-corpus.md))
bridges the two. It implements the *decode-original-as-reference*
pattern: ffprobe + ffmpeg-decode each NR upload to raw YUV, treat
that decoded YUV as the FR "reference", re-encode at multiple CRFs as
the FR "distortion", run the canonical-6 against the pair, emit one
FR corpus row per CRF. This is a known synthetic-distortion VQA
methodology — the same shape LIVE-VQA / LIVE-VQC / KonViD-1k all
use to produce their distortion sets.

## 1. Honest caveat — read this first

The "reference" is the **re-decoded upload, not a pristine master**.
Any artifact already baked into the upload (YouTube VP9
transcoding, capture-chain noise, prior-encode blockiness, banding,
chroma-subsampling losses) propagates into the reference and
therefore into every canonical-6 feature.

What this means for downstream consumers:

- FR scores measure *delta-vs-already-distorted-source*, not
  *delta-vs-pristine-master*.
- A row's `vmaf_score` is a meaningful *relative* signal across CRFs
  for the same source, but the absolute number is not directly
  comparable to a row from BVI-DVC or the Netflix Public drop where
  the reference *is* a master.
- Trainers consuming a mixed corpus (Netflix + BVI-DVC + K150K-via-
  this-adapter) should consider stratifying loss / metrics by the
  `fr_from_nr` provenance flag the adapter writes onto every row.

## 2. Pipeline

```text
KonViD-150k JSONL row                       (NR — distorted MP4 + MOS)
        │
        ▼
ffprobe                                     (probe geometry: W, H, pix_fmt, fps, dur)
        │
        ▼
ffmpeg decode → raw YUV intermediate        (de-facto FR reference)
        │
        ▼
for crf in crf_sweep:                       (default: 18, 23, 28, 33, 38)
    vmaftune.corpus.iter_rows               (existing Phase A pipeline)
        │                                       ├─ encode at (preset, CRF)
        │                                       └─ score canonical-6 vs YUV
        ▼
FR corpus row (CORPUS_ROW_KEYS schema       (1 NR row → N FR rows)
              + nr_source / nr_mos / fr_from_nr)
```

Per-row corpus multiplier: `len(crf_sweep)`. Default sweep is 5
CRFs, so 148,543 K150K rows produce ~742k FR rows on the full pass.

## 3. Adapter API

```python
from vmaftune.fr_from_nr_adapter import NrInputRow, NrToFrAdapter
from vmaftune.corpus import CorpusOptions

adapter = NrToFrAdapter(
    crf_sweep=(18, 23, 28, 33, 38),
    preset="medium",
    scratch_dir=Path("/tmp/fr_from_nr_scratch"),
    keep_intermediate_yuv=False,
    options=CorpusOptions(
        encoder="libx264",
        encode_dir=Path("/tmp/fr_from_nr_encodes"),
        keep_encodes=False,
        vmaf_model="vmaf_v0.6.1",
    ),
)

for row in adapter.run(NrInputRow.from_dict(jsonl_row)):
    write_jsonl_row(row)
```

Output rows match the existing :data:`vmaftune.CORPUS_ROW_KEYS`
schema (no schema bump) plus three provenance keys:

| Key           | Type             | Purpose                                    |
|---------------|------------------|--------------------------------------------|
| `nr_source`   | str              | Path to the NR upload that this row was derived from |
| `nr_mos`      | float \| None    | KonViD MOS for the source upload (passes through unchanged) |
| `fr_from_nr`  | bool (always `True`) | Provenance flag for downstream stratification |

## 4. Subprocess seams (for tests + custom orchestration)

The adapter exposes four runner-injection seams matching the shape
already used by `vmaftune.corpus.iter_rows`:

| Seam            | Wraps      | Used for                          |
|-----------------|------------|-----------------------------------|
| `probe_runner`  | ffprobe    | Geometry probe of the NR source   |
| `decode_runner` | ffmpeg     | Decode-to-YUV step                |
| `encode_runner` | ffmpeg     | FR-sweep encoder (forwarded)      |
| `score_runner`  | vmaf CLI   | FR-sweep scorer (forwarded)       |

Production callers leave all four `None`; the adapter falls back to
`subprocess.run`. The unit tests at
[`tools/vmaf-tune/tests/test_fr_from_nr_adapter.py`](../../tools/vmaf-tune/tests/test_fr_from_nr_adapter.py)
mock all four with no real ffmpeg / vmaf binary needed.

## 5. K150K operator runbook

The full K150K extraction does not run inside any PR. The runbook
script is `ai/scripts/extract_k150k_features.sh`; invoke it overnight
on a workstation with sufficient scratch (~750 GB peak):

```bash
bash ai/scripts/extract_k150k_features.sh \
    --input  .workingdir2/konvid-150k/konvid_150k.jsonl \
    --output runs/k150k_fr_corpus.jsonl
```

The script wraps the adapter with K150K-specific defaults
(`crf_sweep=(18,23,28,33,38)`, `preset=medium`, scratch under
`.workingdir2/k150k-scratch/`, encodes under
`.workingdir2/k150k-encodes/`). Edit the script's defaults block to
override.

Output JSONL is gitignored; only derived training weights ship in-tree
(same posture as the Netflix and BVI-DVC shards —
[ADR-0203](../adr/0203-tinyai-training-data.md),
[ADR-0310](../adr/0310-bvi-dvc-corpus-ingestion.md)).

## 6. Smoke test

```bash
python -m pytest tools/vmaf-tune/tests/test_fr_from_nr_adapter.py -v
```

13 tests cover ffprobe parsing, decode-command shape, adapter
construction validation, single-row sweep, multi-row sweep, scratch
cleanup (default + `keep_intermediate_yuv`), and decode-failure
propagation. Zero filesystem dependency on real ffmpeg / vmaf
binaries — every subprocess call is mocked at the runner seam.

## 7. References

- [ADR-0346](../adr/0346-fr-features-from-nr-corpus.md) — adapter
  pattern decision record.
- [ADR-0325](../adr/0325-konvid-150k-corpus-ingestion.md) — KonViD-150k
  corpus ingestion (the NR shard this adapter consumes).
- [ADR-0309](../adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
  — FR-regressor-v2 trainer (FR consumer).
- [ADR-0323](../adr/0323-fr-regressor-v3-train-and-register.md) —
  FR-regressor-v3 trainer (FR consumer).
- LIVE-VQA: <https://live.ece.utexas.edu/research/quality/live_video.html>
  (accessed 2026-05-09).
- LIVE-VQC: <https://live.ece.utexas.edu/research/LIVEVQC/LIVEVQC.html>
  (accessed 2026-05-09).
- KonViD-1k: <https://database.mmsp-kn.de/konvid-1k-database.html>
  (accessed 2026-05-09).
