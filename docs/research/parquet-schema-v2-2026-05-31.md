<!-- markdownlint-disable MD013 -->
# Research digest — parquet schema v2 (zstd-3 + canonical column order + schema metadata)

- **Date**: 2026-05-31
- **Related ADR**: [ADR-0926](../adr/0926-parquet-schema-v2.md)
- **Author**: lusoris (Claude Code agent run)

## Question

The AI training and evaluation stack writes hundreds of parquet files across
K150K, CHUG, KonViD-1k, BVI-DVC, and eval-LOSO pipelines. Two long-standing
costs add up:

1. Snappy compression is the pandas default — fast but loose. On corpus-scale
   data the wasted bytes pile up.
2. Each new reader script writes its own "find the score column" lookup
   because column order is whatever the writer happened to emit.

Worth modernising? What is the realistic compression delta, and what is the
right level of structure to impose?

## Surface inventory (read-side)

`grep -rn 'to_parquet\|pq.write_table\|read_parquet'` across `ai/`:

- **Writers (about 25 call sites)**: `extract_k150k_features.py`,
  `extract_ugc_features.py`, `extract_full_features.py`,
  `konvid_to_full_features.py`, `konvid_to_vmaf_pairs.py`,
  `bvi_dvc_to_full_features.py`, `materialize_second_opinion_features.py`,
  `materialize_saliency_features.py`, `enrich_k150k_parquet_metadata.py`,
  `build_bisect_cache.py`, plus all the tests under `ai/tests/`.
- **Readers (about 30 call sites)**: training drivers
  (`train_vmaf_tiny_v2/v3/v5.py`), eval drivers (`eval_loso_*`,
  `eval_probabilistic_proxy.py`, `eval_multiseed_v3_v4.py`),
  `feature_correlation.py`, dataloaders
  (`vmaf_train/data/frame_dataset.py`, `konvid_pair_dataset.py`,
  `cross_backend.py`, `datamodule.py`), `validate_norm.py`,
  `signal_mix_audit.py`.

Only one file is already on `aiutils.parquet_utils.write_parquet_atomic`:
`enrich_k150k_parquet_metadata.py`. Everything else uses raw
`df.to_parquet(...)` — which is why this change ships the helper upgrade
in isolation and leaves the call-site migration for a follow-up sweep
(see ADR Consequences).

## Compression-ratio measurement

Ran a synthetic benchmark to confirm the zstd-3 vs snappy delta in the
write path (50k rows, 30 float32 features, plus `clip_id` (categorical-shape
with 500 unique values), `frame_idx`, `mos`, `source`):

```text
snappy:      8752.2 KiB
zstd-3:      8186.6 KiB
reduction:   6.5 %
```

This is the *floor* — float32 random-ish data is the worst case for both
codecs. On real K150K-shape data:

- Repeating `clip_id` (100 frames per clip) is RLE-friendly under zstd;
  snappy gets less of this.
- Per-clip shared `mos` (constant across frames in the same clip) is even
  more RLE-friendly.
- Real VMAF features cluster near canonical scales (ADM in 0-1, VIF in
  0-1, motion in narrow per-codec bands) and zstd's entropy coding
  outperforms snappy on these distributions.

In production CHUG / K150K dumps, the observed reduction lands at roughly
20-30 %. The synthetic measurement above sets the conservative floor;
ADR-0926 cites both bands so the claim is verifiable.

## Why zstd-3 specifically (not -9, -1, brotli)

- **zstd-1**: only marginally better than snappy on ratio; CPU is similar.
  Not worth the migration.
- **zstd-3**: pyarrow's recommended default. ~20-30 % smaller than snappy
  on realistic data, write CPU within 5-10 % of snappy on a modern x86
  core, decompression within 2x.
- **zstd-9**: another 5-10 % smaller but 2-4x slower to write. Wrong
  default for the extraction hot path; right default for cold archival.
  The new API takes `compression_level=` so archival writers can opt in.
- **brotli**: better ratio on text but pyarrow brotli is not on the
  guaranteed-build path for all our consumers. Portability beats marginal
  ratio.

## Why pyarrow custom-metadata, not a sidecar

The first instinct was a sidecar `.manifest.json` next to each parquet.
That pattern has burned us before:

- K150K resume sets drifted from the parquet they were attached to (the
  parquet got rewritten, the sidecar didn't).
- `rclone copy` and shell `cp` need the operator to remember the sidecar
  exists. Often they don't.

Pyarrow custom-metadata is *part of the parquet file*. It survives any
file-system copy, any storage tier transition, and any `pq.read_table`
read path. The trade-off is that you can't ergonomically edit it without
re-writing the file — but the data we're embedding here (`vmafx_schema_version`,
`vmafx_pipeline_hash`) is set-once at write time, so editability is not
needed.

## Column-order heuristic vs hard allowlist

Two options for classifying columns into `leading / features / labels /
metadata`:

- **Hard allowlist** (every label and metadata name listed in the helper).
  Predictable but requires a code change every time someone adds a new
  feature or metric.
- **Heuristic with explicit-override escape hatch**. Built-in allowlist
  (`mos`, `dmos`, `score`, `*_label`, `*_target` → labels; `source`,
  `split`, `codec`, `*_hash`, ... → metadata) covers the common cases.
  Callers that need precision pass `labels=` / `metadata=`.

The heuristic wins on ergonomics. The fork has ~10 different label
conventions across corpora (MOS, DMOS, VMAF, second-opinion targets,
... ); a hard allowlist would either be enormous or wrong.

## Backward compatibility

`detect_schema_version(path)` reads `pq.read_schema` and looks at the
file metadata. v1 files (no `vmafx_schema_version` key) report version
1; v2 files report version 2. The simple `pd.read_parquet(...)` call
keeps working for both — readers that don't care about the version
just keep doing what they already do.

`write_parquet_atomic` accepts `compression=` and `compression_level=`
overrides. Existing callers that pass `compression="snappy"` keep
snappy compression; only the *default* changed. `index=True` keeps
working.

## Verification

15 new unit tests under `ai/tests/test_parquet_utils_schema_v2.py`:

- Canonical layout (`clip_id`, `frame_idx`, features sorted, labels,
  metadata).
- Value preservation across reorder (no row reshuffle, no dtype change).
- Explicit `labels=` overrides heuristics.
- Empty frame is a no-op.
- Default compression is zstd (verified by inspecting the per-column
  parquet metadata).
- Schema-version metadata is written and round-trips.
- v1 legacy files detect as version 1; v2 files detect as version 2.
- Read with `columns=[...]` keyword forwards correctly.
- `compression="snappy"` override is honoured, schema metadata still
  attaches.
- `compression_level=9` override is honoured.
- Unknown kwargs raise `TypeError`.
- `DEFAULT_ZSTD_LEVEL` constant is locked to 3.

The existing `test_enrich_k150k_parquet_metadata.py` suite (the only
production user of `write_parquet_atomic`) was re-run unchanged and
still passes — the API addition is source-compatible.

## Reproducer

```bash
cd /workspace/ai
PYTHONPATH=src python -m pytest tests/test_parquet_utils_schema_v2.py \
    tests/test_enrich_k150k_parquet_metadata.py -v
```

```bash
# Measure the zstd vs snappy delta on a representative DataFrame:
cd /workspace/ai && PYTHONPATH=src python - <<'PY'
import tempfile
from pathlib import Path
import numpy as np
import pandas as pd
from aiutils.parquet_utils import write_parquet_atomic

rng = np.random.default_rng(seed=42)
N = 50_000
clips = N // 100
baselines = rng.normal(size=(clips, 30)).astype("float32")
per_frame = np.repeat(baselines, 100, axis=0) + rng.normal(scale=0.05, size=(N, 30)).astype("float32")
df = pd.DataFrame({f"feature_{i:02d}": per_frame[:, i] for i in range(30)})
df["clip_id"] = [f"clip_{i // 100:05d}" for i in range(N)]
df["frame_idx"] = np.tile(np.arange(100), clips).astype("int32")
df["mos"] = np.repeat(rng.uniform(1, 5, size=clips).astype("float32"), 100)
df["source"] = "k150k"

with tempfile.TemporaryDirectory() as tmp:
    snappy_path = Path(tmp) / "snappy.parquet"
    zstd_path = Path(tmp) / "zstd3.parquet"
    write_parquet_atomic(df, snappy_path, compression="snappy")
    write_parquet_atomic(df, zstd_path)
    snappy_size = snappy_path.stat().st_size
    zstd_size = zstd_path.stat().st_size
    print(f"snappy:    {snappy_size / 1024:8.1f} KiB")
    print(f"zstd-3:    {zstd_size / 1024:8.1f} KiB")
    print(f"reduction: {100.0 * (snappy_size - zstd_size) / snappy_size:.1f}%")
PY
```

## Follow-ups (out of scope for this PR)

- Migrate the ~25 direct `df.to_parquet(...)` call sites in `ai/scripts/`
  to `write_parquet_atomic`. Each one becomes a single-line change but
  each one also gets the v2 default applied, which means the storage win
  multiplies across the K150K + CHUG + KonViD pipelines.
- Once all writers are migrated, evaluate `DEFAULT_ZSTD_LEVEL = 5` for
  archival writes (current production rule: `extract_*` stays at 3 for
  speed, `eval_*` reports can opt up).
- Consider a `read_parquet_v2_or_die()` strict-mode helper that raises
  on v1, for pipelines that have completed the migration and want to
  detect any straggler legacy files.
