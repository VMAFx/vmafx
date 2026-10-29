<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3388: One encoder registry read by the Go and the Python tuners

- **Status**: Accepted
- **Date**: 2026-10-29
- **Deciders**: Lusoris (ledger vmafx-encoder-registry-20261029, per vmafx-encoder-coverage-20261029)
- **Tags**: `vmaf-tune`, `vmafx-tune`, `codegen`, `encoders`, `fork-local`

## Context

Before this decision VMAFx described its encoders in four places:

- the Python adapters in `tools/vmaf-tune/src/vmaftune/codec_adapters/`
  (`_REGISTRY`, one frozen dataclass per codec);
- `pkg/encoder/adapter.go` (`adapterRegistry`, the Go `vmafx-tune` windows,
  defaults and preset lists);
- `pkg/codecadapter/codecadapter.go` (the Go port of the Python adapter
  contract, with its own copies of the same facts);
- `_ABSOLUTE_CRF_RANGE_BY_NAME` in `tools/vmaf-tune/src/vmaftune/bisect.py`
  (the bisect search window of [ADR-0538](0538-premium-vmaf-target-defaults-and-bisect.md)).

The copies had already drifted: four Go adapters (`h264_nvenc` family, the
AMF family, `libaom-av1`, `libsvtav1`) reported an empty `AdapterVersion`
where the Python adapters carry `"1"` or `"2"`, so cache keys built from the
version differed between the two tools. Each new encoder had to be added in
up to four files, and nothing listed the encoders VMAFx does *not* support
yet, their licence or their upstream state.

The coverage plan (order vmafx-encoder-registry-20261029 and its addenda)
needs one table that also holds every gap: every family (AV1, HEVC, H.264,
VVC, VP9, VP8, EVC, AV2, AVS3, mezzanine and intra codecs, GPU texture
codecs, legacy distortion sources, neural codecs), each with a paradigm,
use-case tags, a licence class and the date its upstream state was checked.

## Decision

One definition file, `api/vmafx-encoders.toml`, holds every encoder row and
every `ADAPTER@VARIANT` row ([ADR-0644](0644-vmaf-tune-codec-runtime-variants.md)
variants: fork builds and tuning profiles). The existing generator
(`scripts/codegen/vmafx-api.py`, [ADR-1852](1852-vmafx-api-redesign.md))
validates it (`scripts/codegen/vmafx_api/encoders.py`) and writes three
outputs (`emit_encoders.py`):

- `pkg/encoderregistry/registry.gen.json`, embedded by the Go package
  `pkg/encoderregistry`; `pkg/encoder` builds its adapter table from it and
  `pkg/codecadapter` fills each adapter's facts from it;
- `tools/vmaf-tune/src/vmaftune/codec_adapters/registry.gen.json`, a
  byte-identical copy read by `codec_adapters/registry.py`; the bisect window
  is the row's `absolute_range`;
- `docs/usage/encoder-registry.md`, the table for readers.

A row is `implemented` exactly when it names the surfaces (`go`, `python`)
that run it and carries an `[encoders.adapter]` table: quality knob, quality
window, absolute range, default, presets, preset style, adapter version, probe
settings and capability flags. Argv construction stays in code; only facts
move. Every other row is `planned`, `listed` (proprietary or reference
software, no implementation intended) or `decision` (implementation waits for
an operator ruling, for example a licence-gated SDK).

`vmafx-api.py --check` (Meson test `test_vmafx_api_generated_current`) fails
when an output differs from the definition. `tools/vmaf-tune/tests/test_encoder_registry.py`
holds every Python adapter equal to its row, and
`pkg/codecadapter/registry_test.go` and `pkg/encoder` tests do the same for Go.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One TOML definition, generated JSON embedded in Go and shipped in the Python package (chosen) | One source; both languages read the same bytes; reuses the ADR-1852 generator and its drift gate; gap rows cost nothing at run time | Generated files are committed; the facts leave the adapter files | — |
| Python adapters as the source, Go generated from them | No new file format | Go generation would import Python code at build time; gap rows have no adapter to live in | Gaps and licence data need a table, not code |
| Go `pkg/encoder` as the source, Python reads a Go-emitted file | Go is the successor tool | The Python tool would depend on a Go build; the second Go list (`pkg/codecadapter`) stays | Couples the Python wheel to the Go toolchain |
| Keep the copies, add a cross-language equality test only | Smallest change | Every new encoder still lands in four places; no home for gap rows | Detects drift but leaves the duplication (HISS-19) |
| YAML instead of TOML | Familiar | Every other generator definition here is TOML (`core/api/vmafx.toml`, `api/vmafx-platform.toml`) | A second definition format is a defect (HISS-19) |

## Consequences

- **Positive**: one place to add an encoder or a gap; the Go/Python
  `AdapterVersion` drift is fixed and cannot return; the bisect window is no
  longer a separate list; the docs table cannot go stale.
- **Negative**: changing a quality window now means editing the TOML and
  running `vmafx-api.py --write`; the adapter dataclasses still carry their
  field values, held equal by the test rather than read at import time.
- **Neutral / follow-ups**: the coverage matrix (gap rows with licence class,
  upstream state and use-case tags), the epics on the existing milestones and
  the per-use-case ranking land as later slices on this format. Rows whose
  implementation needs a licence ruling stay `decision` until the ledger
  holds one.

## References

- Order vmafx-encoder-registry-20261029 and addenda 1747, 1748, 1750, 1752
  (paraphrased: one central registry read by both tuners, a full family
  matrix with licence and upstream state, epics on existing milestones,
  ranking by use-case class).
- [ADR-0644](0644-vmaf-tune-codec-runtime-variants.md) runtime variants;
  [ADR-0538](0538-premium-vmaf-target-defaults-and-bisect.md) bisect window;
  [ADR-1852](1852-vmafx-api-redesign.md) generator.
- Research digest: [docs/research/3388-central-encoder-registry-2026-10-29.md](../research/3388-central-encoder-registry-2026-10-29.md).
