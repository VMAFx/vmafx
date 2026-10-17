<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3311: Stream metadata field list seeded from the Pelorus registry

- **Status**: Accepted
- **Date**: 2026-10-13
- **Deciders**: Lusoris
- **Tags**: api, pelorus, interop, schema

## Context

VMAFx plans to report per-frame stream metadata next to each score: the coded
size, the average quantiser, the picture type and other decode-side facts of
the distorted bitstream (VMAFx/vmafx#2271, milestone 1.1). The Pelorus
telemetry field registry (`libpelorus/schema/telemetry-fields.json`, schema
`pelorus/telemetry-fields/1`) already names the same facts; 17 of its 31 rows
are marked `decoder: shared`, meaning a decoder-side reader such as VMAFx can
report them. A consumer that joins Pelorus telemetry with VMAFx scores needs
both projects to spell, type and measure each shared field the same way.

Pelorus already holds a parity check, `scripts/check-telemetry-field-parity.py`,
that compares its registry with a VMAFx field list. Without a VMAFx file it
exits 77 (skipped), so the agreement is unchecked. #2271 itself is blocked
by #2311 and #2510; waiting for it leaves the names open until the reporting code
is written, which is when a mismatch is most expensive to fix.

## Decision

1. VMAFx keeps its field list as `api/stream-metadata-fields.json`, schema
   `vmafx/stream-metadata-fields/1`, ahead of the reporting code.
2. The list is seeded from the Pelorus registry pinned by commit
   (`seeded_from`). Each row VMAFx reports copies the registry row's key,
   JSON type, unit and range and is marked `pelorus: shared`; a field Pelorus
   lacks is `vmafx_only` and may not reuse a registry key.
3. Every registry shared key VMAFx does not report is listed in
   `not_carried`, so the two lists stay comparable key by key.
4. The list is append-only: no key is renamed, retyped, given another unit or
   reused. A change that cannot be an addition takes schema version 2.
5. `scripts/ci/tests/test_stream_metadata_fields.py` checks the file's shape
   in VMAFx; Pelorus runs its parity check against a copy of the file pinned
   to a VMAFx commit.

The first list carries nine fields (`display_index`, `decode_index`,
`frame_bytes`, `avg_qp`, `qp_scale`, `picture_type`, `key_frame`, `codec`,
`coded_bit_depth`) and lists eight as not carried (`avg_qp_norm`, `reference`,
`shown`, `qp_map`, `granularity`, `map_cols`, `map_rows`, `block_size_log2`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Field list in VMAFx, seeded from the pinned registry, with `not_carried` (chosen) | Names fixed before the code; Pelorus check runs; VMAFx owns its own surface | Two files to keep in step | — |
| Vendor the Pelorus registry into VMAFx and read it directly | One source of names | VMAFx would carry encoder-only rows and the interop bit layout it never reads; no place to state what VMAFx does not report | Couples VMAFx's output schema to Pelorus's encoder registry |
| Wait for #2271 and name the fields with the reporting code | No file before it is used | Names stay open while #2311 and #2510 block; the parity check keeps skipping | A mismatch found after the code lands is a breaking rename |
| Generate the VMAFx file from the registry at build time | No copying by hand | A registry change would change VMAFx's output schema without a VMAFx review; breaks append-only | VMAFx's public names must change only through a VMAFx pull request |

## Consequences

- **Positive**: the names #2271 will report are fixed and checked now; a
  Pelorus registry change that breaks parity fails the Pelorus check instead
  of reaching users.
- **Negative**: moving `seeded_from` to a newer Pelorus commit is a manual
  step, and a new Pelorus shared key needs a VMAFx pull request that adds it to
  `fields` or `not_carried`.
- **Neutral / follow-ups**: Pelorus pins a copy of the merged file as
  `libpelorus/test/fixtures/vmafx-stream-metadata-fields.json`, after which its
  parity check passes instead of skipping. #2271 emits these fields.

## References

- [Stream metadata field list](../api/stream-metadata-fields.md)
- [Pelorus interop ABI (vendored)](../api/pelorus-interop.md),
  [ADR-1113](1113-vendor-pelorus-interop-abi.md)
- Pelorus `libpelorus/schema/telemetry-fields.json` and
  `scripts/check-telemetry-field-parity.py` at VMAFx/pelorus
  `ec45e24d7342df908a2545e3a1d6dc7516d27475`
- Source: operator order of 2026-10-13 to land the field list as its own pull
  request ahead of #2271 (paraphrased)
