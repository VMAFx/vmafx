<!-- markdownlint-disable MD013 -->
# Run-result timeline — `api/run-result-timeline.schema.json`

VMAFx plans to write one report format for a scoring run instead of letting
alignment, fields, masks and bits-per-pixel each invent their own
([VMAFx/vmafx#2452](https://github.com/VMAFx/vmafx/issues/2452), RC5): a
versioned JSON document that carries the run's provenance, its per-frame
scores, its per-window statistics and its events - alignment decisions, drops,
repeats, freezes, switches, masked areas - each located by one time key that
can carry a frame index, a timestamp or a timecode.

[`api/run-result-timeline.schema.json`](https://github.com/VMAFx/vmafx/blob/master/api/run-result-timeline.schema.json)
is the contract of that document, and this page is its human reference. It is
the shape only, not the reporting code: no CLI flag, C call or server endpoint
writes the document yet. The writer/reader C ABI and the identity checks land
in the follow-up PRs of the same issue; the converters from the existing
XML/JSON/CSV outputs follow them.

## What the document holds

| Section | Carries | Contract |
| --- | --- | --- |
| `schema_version` | The schema version the document was written against (`1` today) | A reader refuses an unknown version by name |
| `provenance` | The provenance record of the context that produced the scores (`vmafx_context_provenance_json()`, RC4 WP5, #2288) | The authoritative field contract is the OpenAPI [`Provenance` schema](../../api/openapi/vmafx-server-v1.yaml) and the `VmafxProvenance` definition in [`core/api/vmafx.toml`](../../core/api/vmafx.toml); the timeline schema pins the `digest` identity anchor |
| `scoring_configuration` | The scoring-option surface in effect (#2258, WP8) | The `scoring` component group of the same OpenAPI document is the single source |
| `frames` | The per-frame series: a time key plus the frame's per-feature scores | The time key carries at least one of frame index, timestamp, timecode |
| `windows` | Per-window statistics: a start, an end, the statistic's name and its value | Ranges are the half-open interval the writer knows |
| `events` | The run's events: a type from the additive vocabulary, a required time key, optional facts | An event without a time key is refused by name |

The time key mirrors what a run actually knows: a frame score carries the
0-based display-order frame index (the stream-metadata `display_index` of
[`api/stream-metadata-fields.json`](https://github.com/VMAFx/vmafx/blob/master/api/stream-metadata-fields.json)),
a wall-clock event carries the nanosecond timestamp, and a writer may add the
SMPTE-style timecode when the run has one.

## Versioning

The document evolves additively under the project's
[Append-Only ABI](../principles.md) invariant (HISS-14): after a release, the
schema gains members, and never removes, renames or retypes one. `schema_version`
is the integer a document was written against; a reader that meets a version it
does not know refuses the document by name rather than guessing. The acceptance
fixtures that pin today's shape live in
[`api/testdata/run-result-timeline/`](https://github.com/VMAFx/vmafx/tree/master/api/testdata/run-result-timeline)
and run in
[`scripts/ci/tests/test_run_result_timeline_schema.py`](https://github.com/VMAFx/vmafx/blob/master/scripts/ci/tests/test_run_result_timeline_schema.py)
(the `tooling` suite):

```bash
python -m pytest scripts/ci/tests/test_run_result_timeline_schema.py -q
```

## Not in the skeleton

The skeleton pins identities, not every field contract. The provenance record's
fields and the scoring-option surface stay with their single sources (the
OpenAPI document and `core/api/vmafx.toml`) instead of being copied here, so
the two contracts cannot drift; the timeline schema constrains what makes a
timeline attributable and addressable. The richer verdict and facts machinery
of #2311 extends this schema in 1.1.
