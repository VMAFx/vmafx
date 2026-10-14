<!-- markdownlint-disable MD013 -->
# Stream metadata field list — `api/stream-metadata-fields.json`

VMAFx plans to report per-frame stream metadata next to each score: the
decode-side facts of the distorted bitstream, such as the frame's coded size,
its average quantiser and its picture type
([VMAFx/vmafx#2271](https://github.com/VMAFx/vmafx/issues/2271)). The
[Pelorus](https://github.com/VMAFx/pelorus) telemetry field registry already
names the same facts for its own pipeline. So that a consumer can join the two
streams without a mapping table, every field the two projects share is
spelled, typed and measured the same way in both.

[`api/stream-metadata-fields.json`](https://github.com/VMAFx/vmafx/blob/master/api/stream-metadata-fields.json)
is the VMAFx side of that agreement. It is the list of field names, not the
reporting code: no CLI flag or library call emits these fields yet. The
reporting lands with #2271 and uses these names.

## What the file holds

| Key | Meaning |
| --- | --- |
| `schema` | `vmafx/stream-metadata-fields/1`. A reader that does not know the version refuses the file. |
| `seeded_from` | The Pelorus registry the list was seeded from, pinned to a commit: `VMAFx/pelorus@<40-hex commit>:libpelorus/schema/telemetry-fields.json`. |
| `fields` | One row per field VMAFx reports: `key`, `json` (JSON type), `unit`, `range` and `pelorus`. |
| `not_carried` | The keys of the Pelorus registry's shared rows that VMAFx does not report. |

The `pelorus` column of a row is `shared` when the Pelorus registry has a
shared row with the same key; the row's `json`, `unit` and `range` are then
copied from that registry row. A field Pelorus does not have is `vmafx_only`,
and its key must not be any key of the registry.

## The fields

| Key | JSON | Unit | Range | Pelorus |
| --- | --- | --- | --- | --- |
| `display_index` | integer | frame | 0-based display order | shared |
| `decode_index` | integer | frame | 0-based coding order | shared |
| `frame_bytes` | integer | byte | coded frame, headers included, container excluded | shared |
| `avg_qp` | number | `qp_scale` | inside the `qp_scale` range | shared |
| `qp_scale` | string | none | `slice_qp` \| `av1_qindex` \| `vp9_qindex` \| `av1_encoder_qp` | shared |
| `picture_type` | string | none | `i` \| `p` \| `b` | shared |
| `key_frame` | boolean | none | random-access point | shared |
| `codec` | string | none | `h264` \| `hevc` \| `vvc` \| `av1` \| `vp9` | shared |
| `coded_bit_depth` | integer | bit | `8` \| `10` \| `12` | shared |

`avg_qp` is measured on the scale `qp_scale` names: H.264, HEVC and VVC slice
QP, the AV1 or VP9 quantiser index, or the AV1 encoder's QP. Compare two
`avg_qp` values only when their `qp_scale` is the same.

## Not carried

VMAFx does not report these shared Pelorus keys: `avg_qp_norm`, `reference`,
`shown`, `qp_map`, `granularity`, `map_cols`, `map_rows` and
`block_size_log2`. Naming them keeps the two lists comparable: a Pelorus
shared key is either a VMAFx field or listed here, never missing silently.
Moving a key from `not_carried` to `fields` is an ordinary addition.

## Changing the list

The list is append-only. A key is never renamed, retyped, given another unit
or reused for another meaning; a field that changes meaning gets a new key,
and a change that cannot be an addition takes a new schema version
(`vmafx/stream-metadata-fields/2`). A new Pelorus shared key goes into
`fields` or `not_carried` when VMAFx next moves `seeded_from` to a newer
Pelorus commit.

Two checks hold the file:

- `scripts/ci/tests/test_stream_metadata_fields.py` (suite `tooling`) checks
  the shape: the schema version, a pinned `seeded_from`, every column of every
  row, snake_case keys, known JSON types and Pelorus classes, no duplicate key
  and no key both carried and not carried.

  ```bash
  python3 -m pytest scripts/ci/tests/test_stream_metadata_fields.py
  ```

- The Pelorus repository's `scripts/check-telemetry-field-parity.py` compares
  the two files: every `shared` row must be a registry shared row with the
  same JSON type and unit, every registry shared key must be a `shared` row or
  in `not_carried`, and no `vmafx_only` key may reuse a registry key. It exits
  0 when they agree, 1 on a mismatch, 2 on a malformed file and 77 (skipped) when it has no VMAFx
  copy to read. Run it from a Pelorus checkout against this file:

  ```bash
  python3 -I scripts/check-telemetry-field-parity.py --root . \
      --vmafx /path/to/vmafx/api/stream-metadata-fields.json
  ```

The decision and the alternatives are in
[ADR-3311](../adr/3311-stream-metadata-field-list.md).
