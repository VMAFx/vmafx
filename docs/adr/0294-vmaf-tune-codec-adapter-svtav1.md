# ADR-0294: vmaf-tune codec adapter for SVT-AV1

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: `tools`, `vmaf-tune`, `codec`, `av1`, `fork-local`

## Context

`vmaf-tune` ([ADR-0237](0237-quality-aware-encode-automation.md)) ships
a codec-adapter registry so the corpus harness can sweep multiple
encoders without branching on codec identity in the search loop. Phase
A wired `libx264` first; the roadmap calls out `libsvtav1` (the modern
AV1 reference encoder, jointly maintained by Intel / Netflix / Meta) as
the next adapter so the harness can build cross-codec corpora that
match `fr_regressor_v2`'s closed `CODEC_VOCAB`
([ADR-0235](0235-codec-aware-fr-regressor.md)).

SVT-AV1 differs from x264 in three load-bearing ways the adapter has
to encode as data, not behaviour:

1. **CRF range is 0..63**, not 0..51. A naive copy of the x264 adapter
   would silently accept invalid AV1 CRF values or reject legal ones.
2. **Presets are integers 0..13**, not named tiers. The harness CLI and
   the corpus row schema both use string preset names; the adapter has
   to round-trip the name into an integer for FFmpeg's `-preset` argv
   slot while preserving the name in the JSONL row.
3. The "slowest..fastest" convention is the same as x264, but the
   semantically equivalent integers are different — `medium` is `7`,
   not `5`, because `7` is the upstream SVT-AV1 default.

## Decision

Add `tools/vmaf-tune/src/vmaftune/codec_adapters/svtav1.py` defining
`SvtAv1Adapter` and a closed `PRESET_NAME_TO_INT` table. Wire it into
the registry as `"libsvtav1"`. Translate preset names to integers via
an optional `ffmpeg_preset_token()` adapter hook the corpus loop calls
when present; the existing libx264 path keeps using the name verbatim.
The Phase A informative CRF window is `(20, 50)`; absolute-range
validation (`0..63`) fires before the Phase A check so users see the
right error class.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Accept integer presets directly via the CLI | Matches SVT-AV1 native shape | Forces every Phase B/C consumer to special-case AV1 vs x264 in the row schema | Breaks the "harness must not branch on codec identity" invariant from ADR-0237. |
| Hard-code the name->int translation inside `encode.build_ffmpeg_command` | One fewer adapter method | Couples the encode driver to AV1-specific knowledge; doesn't scale to libvvenc / libvpx-vp9 next | Rejected — adapters own their codec quirks. |
| Skip the Phase A window on AV1 (use full `0..63`) | Lets users sweep the entire encoder range | The fr_regressor_v2 training corpus only carries informative labels in the central window; full-range sweeps would produce rows with degenerate VMAF=0 or VMAF=100 | Pin `(20, 50)` for Phase A; widen later if a downstream consumer needs it. |

## Consequences

- **Positive**: corpus harness can build SVT-AV1 corpora today; the
  same Phase B/C loaders that read libx264 rows read libsvtav1 rows
  unchanged. `fr_regressor_v2` consumes the new corpora directly via
  its existing `libsvtav1` slot in `CODEC_VOCAB` (index 2).
- **Negative**: the harness now carries one optional adapter hook
  (`ffmpeg_preset_token`) the libx264 path doesn't use. The cost is
  five lines in `corpus.py` and a one-codec divergence test must
  guard the row-vs-argv preset round-trip.
- **Neutral / follow-ups**:
  - `parse_versions` now matches an SVT-AV1 banner pattern in addition
    to the x264 one. Future codec adapters will extend the same list.
  - Integration smoke against a real `ffmpeg -c:v libsvtav1` is gated
    to the CI runner that ships libsvtav1; local-developer smoke
    relies on the mocked-subprocess unit tests.

## References

- [ADR-0237: vmaf-tune scaffold](0237-quality-aware-encode-automation.md).
- [ADR-0235: codec-aware FR regressor (`fr_regressor_v2`)](0235-codec-aware-fr-regressor.md).
- SVT-AV1 user guide — preset / CRF documentation: <https://gitlab.com/AOMediaCodec/SVT-AV1/-/blob/master/Docs/Parameters.md>.
- FFmpeg libsvtav1 wrapper: <https://ffmpeg.org/ffmpeg-codecs.html#libsvtav1>.
- Source: `req` ("Add an SVT-AV1 codec adapter to vmaf-tune, mirroring
  the existing x264 adapter ... AV1 CRF range 0-63 ... preset 0-13").
