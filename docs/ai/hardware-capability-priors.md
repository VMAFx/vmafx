<!-- markdownlint-disable MD013 MD060 -->
# Hardware-capability priors

Operator-facing reference for the per-architecture GPU encode
fingerprint table that the corpus-ingest pipeline merges into
training rows. Owned by [ADR-0335](../adr/0335-hardware-capability-priors.md);
methodology written up in
[`docs/research/0088-hardware-capability-priors-2026-05-08.md`](../research/0088-hardware-capability-priors-2026-05-08.md).

## What the table is

`ai/data/hardware_caps.csv` is a small (~7 row) table of
**capability metadata** for the GPU video-encode generations the
fork's corpus encounters today. Each row describes what one
architecture *can* do — codecs it can emit, resolution caps per
codec, encoder-block count, tensor-core / NPU presence, minimum
production driver — sourced from primary vendor documentation.

The table is consumed by
[`ai/scripts/hardware_caps_loader.py`](../../ai/scripts/hardware_caps_loader.py)
at corpus-ingest time. The loader's
`cap_vector_for(encoder, encoder_arch_hint)` function returns a flat
dict of `hwcap_*` columns the ingest script merges into each corpus
row before the parquet write. The trainer then has structural
priors like "this generation supports AV1" or "this generation has
tensor cores" available without ever measuring throughput.

## What the table is NOT

Not a benchmark database. The table contains **zero** throughput,
latency, watts, TDP, fps, or quality numbers. The contributor-pack
research digest's category-1 NO-GO finding established that
shipping vendor-published performance numbers leaks biased priors
into the FR-regressor — vendors run their own benchmarks under
favourable conditions and the numbers are not comparable across
generations or vendors. The trainer learns performance from the
corpus's own measured rows; capability metadata only describes the
search space.

If you find yourself wanting to add `nvenc_av1_max_throughput_fps`
or similar, stop and re-read the digest. The right place for that
data is a corpus row produced by the fork's own `vmaf-tune`
pipeline, not a static CSV.

## Schema

| Column                       | Type                       | Notes                                               |
| ---------------------------- | -------------------------- | --------------------------------------------------- |
| `arch_name`                  | kebab-case string          | e.g. `battlemage`, `rdna4`, `blackwell`             |
| `vendor`                     | `intel` \| `amd` \| `nvidia` | rejected if anything else                          |
| `gen_year`                   | int                        | first retail product year                           |
| `codecs_supported`           | `\|`-separated tokens      | only HW-encode codecs (decode-only excluded)        |
| `max_resolution_per_codec`   | `\|`-separated `codec=WxH` | one entry per codec in `codecs_supported`           |
| `encoding_blocks`            | int >= 1                   | independent encode engines on the highest-tier SKU  |
| `tensor_cores`               | bool                       | matrix / tensor / XMX path present                  |
| `npu_present`                | bool                       | on-die NPU separate from GPU tensor units           |
| `driver_min_version`         | string                     | oldest production driver branch                     |
| `source_url`                 | https URL                  | vendor primary doc only (Wikipedia rejected)        |
| `verified_date`              | ISO-8601                   | date the row was verified against `source_url`      |

## Architectures shipped on 2026-05-08

| arch         | vendor | gen_year | encode codecs    | role                      |
| ------------ | ------ | -------- | ---------------- | ------------------------- |
| alchemist    | intel  | 2022     | h264 hevc av1    | predecessor of Battlemage |
| battlemage   | intel  | 2024     | h264 hevc av1    | named target              |
| rdna3        | amd    | 2022     | h264 hevc av1    | predecessor of RDNA4      |
| rdna4        | amd    | 2025     | h264 hevc av1    | named target              |
| ada-lovelace | nvidia | 2022     | h264 hevc av1    | predecessor of Blackwell  |
| blackwell    | nvidia | 2025     | h264 hevc av1    | named target              |

Each row's primary verification source URL lives directly in the
CSV's `source_url` column — reviewers and future operators can
follow the link to confirm any claim before relying on it.

### Why Hopper is absent

NVIDIA Hopper (H100 / H200) ships zero NVENC engines on the
data-centre SKUs and is therefore out of scope for an
encode-capability fingerprint. Including a row with
`encoding_blocks=0` would be misleading; the loader rejects it
schema-side.

## Ingest contract

```python
from ai.scripts.hardware_caps_loader import (
    HardwareCapsTable,
    cap_vector_for,
)

caps = HardwareCapsTable.load_default()

for encode_row in corpus_rows:
    fingerprint = cap_vector_for(
        caps,
        encoder=encode_row["encoder"],            # e.g. "av1_nvenc"
        encoder_arch_hint=encode_row.get("arch"), # e.g. "blackwell"
    )
    encode_row.update(fingerprint)
```

The returned dict always carries the same keys (`hwcap_known`,
`hwcap_arch_name`, …) regardless of whether the architecture was
resolved. Unresolved rows get `hwcap_known=0` and `None` for every
other field — downstream parquet writers serialise these as nulls
so the column shape is stable across the dataset.

The loader only fingerprints hardware encoders (`*_nvenc`,
`*_amf`, `*_qsv`). CPU-only encoders (`libx264`, `libx265`,
`libsvtav1`, …) return the same blank vector with `hwcap_known=0`.

## Adding a new architecture

1. Identify the primary vendor doc — NVIDIA Video Codec SDK release
   notes, AMD GPUOpen / AMF docs, Intel oneVPL or Arc product pages.
   Wikipedia, community wikis, and third-party reviews are NOT
   acceptable. `https://...` only.
2. Confirm every required column from primary text. If any column
   cannot be verified, do **not** ship the row — drop it and open
   an issue noting which capability is unverified.
3. Append the row to `ai/data/hardware_caps.csv`. The schema check
   in `ai/tests/test_hardware_caps.py` rejects empty fields,
   missing `codec=WxH` pairs, non-vendor source domains, and
   non-ISO `verified_date`.
4. Run the round-trip test:

   ```bash
   pytest ai/tests/test_hardware_caps.py
   ```

5. Bump the row's `verified_date` to the date you read the source
   doc, not the date you opened the PR.

## Re-verification cadence

The `verified_date` column is the only operational hint about
freshness. There is no scheduled rotation today; the table is
small enough that a one-shot re-walk every 12 months (or whenever
a new GPU generation lands) is the expected pattern. Rows whose
`verified_date` falls more than a year behind the head of master
should be re-walked or dropped.

## Cross-references

- [ADR-0335](../adr/0335-hardware-capability-priors.md) — decision record.
- [`docs/research/0088-hardware-capability-priors-2026-05-08.md`](../research/0088-hardware-capability-priors-2026-05-08.md) — research digest with the NO-GO finding on benchmark numbers.
- [`ai/data/hardware_caps.csv`](../../ai/data/hardware_caps.csv) — the table itself.
- [`ai/scripts/hardware_caps_loader.py`](../../ai/scripts/hardware_caps_loader.py) — loader + ingest helper.
- [`ai/tests/test_hardware_caps.py`](../../ai/tests/test_hardware_caps.py) — round-trip / schema / vector tests.
