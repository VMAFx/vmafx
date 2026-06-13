# `vmaf-tune --resolution-aware`

`vmaf-tune corpus` enables resolution-aware scoring by default. For each
source job, the tool selects the VMAF model from the encode height and records
the effective model in every JSONL row's `vmaf_model` field.

## Decision Rule

| Encode height | VMAF model |
| --- | --- |
| `>= 2160` | `vmaf_4k_v0.6.1` |
| `< 2160` | `vmaf_v0.6.1` |

The rule is height-only. Width is accepted by the Python API for symmetry, but
the current implementation deliberately ignores it because the fork does not
ship separate anamorphic, 1440p, 720p, or SD models.

## CLI Usage

Default mode:

```shell
vmaf-tune corpus \
  --source ref_4k.yuv --width 3840 --height 2160 --pix-fmt yuv420p \
  --framerate 24 --duration 10 \
  --preset medium --crf 22 \
  --output corpus.jsonl
```

Rows from that job carry:

```json
{"vmaf_model": "vmaf_4k_v0.6.1"}
```

To reproduce a legacy single-model corpus, disable the automatic selector and
pass the model explicitly:

```shell
vmaf-tune corpus \
  --source ref_4k.yuv --width 3840 --height 2160 --pix-fmt yuv420p \
  --preset medium --crf 22 \
  --no-resolution-aware --vmaf-model vmaf_v0.6.1 \
  --output corpus_legacy.jsonl
```

## Python API

```python
from vmaftune.resolution import (
    crf_offset_for_resolution,
    select_vmaf_model,
    select_vmaf_model_version,
)

assert select_vmaf_model_version(3840, 2160) == "vmaf_4k_v0.6.1"
assert select_vmaf_model_version(1920, 1080) == "vmaf_v0.6.1"
assert select_vmaf_model(3840, 2160).name == "vmaf_4k_v0.6.1.json"
assert crf_offset_for_resolution(1280, 720) == 2
```

`crf_offset_for_resolution(width, height)` is a search-seeding helper for
bisect and ladder code:

| Encode height | CRF offset |
| --- | --- |
| `>= 2160` | `-2` |
| `>= 1080` and `< 2160` | `0` |
| `>= 720` and `< 1080` | `+2` |
| `< 720` | `+4` |

The offset is not a quality gate. It is a conservative starting hint for CRF
searches that need to traverse multiple resolution rungs.

## Operational Notes

- `vmaf_model` is per-row metadata. Mixed-resolution corpora may contain both
  `vmaf_v0.6.1` and `vmaf_4k_v0.6.1`; downstream consumers must group or
  filter by that field instead of assuming one model per corpus file.
- `--no-resolution-aware` is the compatibility escape hatch for reproducing
  older corpora or experiments that intentionally used one model everywhere.
- Invalid dimensions raise `ValueError` in the Python API and are rejected by
  the CLI's existing required `--width / --height` path.

## See Also

- [ADR-0289](../adr/0289-vmaf-tune-resolution-aware.md) — decision record.
- [Research-0064](../research/0064-vmaf-tune-resolution-aware.md) — model
  selection and CRF-offset rationale.
- [`vmaf-tune.md`](vmaf-tune.md#resolution-aware-mode) — umbrella usage page.
