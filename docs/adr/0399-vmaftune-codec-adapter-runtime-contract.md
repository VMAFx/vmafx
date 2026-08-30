<!-- markdownlint-disable MD060 -->
# ADR-0399: `vmaf-tune` codec-adapter contract becomes a runtime contract (HP-1)

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: Lusoris
- **Tags**: tooling, codec, automation, fork-local, bug-fix

## Context

[ADR-0237](0237-quality-aware-encode-automation.md) framed the `vmaf-tune`
codec adapter as a Protocol with a per-codec
``ffmpeg_codec_args(preset, quality)`` slice so the search loop never
branches on codec identity. Phase A wired sixteen adapters into
`codec_adapters._REGISTRY` (libx264, libaom-av1, libx265, three NVENC,
three AMF, three QSV, two VideoToolbox, libvvenc, libsvtav1).

Phase A's audit (HP-1) found the contract was **docstring-only**: every
live argv composition site bypassed the adapter and emitted a hardcoded
``["-c:v", req.encoder, "-preset", req.preset, "-crf", str(req.crf)]``
shape. Three call sites had this pattern:

1. `tools/vmaf-tune/src/vmaftune/encode.py::build_ffmpeg_command` —
   the Phase A grid sweep + corpus.iter_rows path.
2. `tools/vmaf-tune/src/vmaftune/per_shot.py::_segment_command` —
   the Phase D per-shot ladder builder.
3. `tools/vmaf-tune/src/vmaftune/corpus.py::iter_rows` — composes its
   `EncodeRequest` and routes through `run_encode`, which uses (1).

Result: 11 of 16 adapters were non-functional for live grids — the
ffmpeg invocation either silently mis-encoded (ignoring native flags
like `-cq`, `-global_quality`, `-q:v`, `-qp`) or crashed
(`libaom-av1`, which has no `-preset` flag). Eleven adapters did not
even ship a `ffmpeg_codec_args` method; only x264, libaom (with a
non-conforming tuple shape), and the two VideoToolbox adapters did.

## Decision

Promote `ffmpeg_codec_args` from a documented-only contract to a
**runtime contract**. Concretely:

1. Add `ffmpeg_codec_args(preset: str, quality: int) -> list[str]` to
   every adapter that didn't ship one (libx265, libsvtav1, libvvenc,
   the three NVENC adapters, the three AMF adapters, the three QSV
   adapters). Each returns the codec-correct argv slice — including
   `-cpu-used` (libaom), `-cq` (NVENC), `-global_quality` (QSV),
   `-quality + -rc cqp + -qp_i + -qp_p` (AMF), `-qp` (VVenC),
   `-realtime + -q:v` (VideoToolbox).
2. Normalise libaom's existing slice to match the dispatcher contract
   (`-c:v` prefix included, `-an` dropped — audio handling for raw YUV
   inputs is automatic) and return `list[str]` rather than `tuple`.
3. Replace the hardcoded ``-c:v ... -preset ... -crf ...`` literal in
   `encode.build_ffmpeg_command` with a dispatcher that calls
   `get_adapter(req.encoder).ffmpeg_codec_args(req.preset, req.crf)`.
   A legacy fallback path keeps the historic shape for unregistered
   encoders so callers that bypass the registry stay invocable.
4. Replace the equivalent hardcode in `per_shot._segment_command` with
   the same dispatcher seam, taking the adapter object directly so
   the per-shot ladder can encode against any registered codec.
5. Add a per-adapter live-encode smoke test
   (`tests/test_encode_dispatcher_per_adapter.py`) that parametrises
   across every entry in `_REGISTRY`, mocks `subprocess.run`, captures
   the composed argv, and asserts the codec-correct flags are present.
   The fixture table is gated by a meta-test that fails if a new
   adapter lands without a matching row.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Dispatcher pivot via `get_adapter().ffmpeg_codec_args()`** *(chosen)* | Honours the existing Protocol; one source of truth per codec; legacy fallback keeps unregistered encoders working; smoke-tested across all 16 adapters | Adds an indirection in the hot path of `build_ffmpeg_command` (one dict lookup + one method call per encode invocation — negligible vs. ffmpeg startup). | — |
| Keep the hardcode and special-case the codecs that need different flags | Minimal indirection; matches the pre-Phase A shape | Defeats the codec-adapter design (ADR-0237's "search loop never branches on codec identity" invariant); each new codec re-opens `encode.py`; libaom would still crash | Rejected — directly contradicts ADR-0237's stated invariant |
| Bake the codec slice into `EncodeRequest` so the harness doesn't dispatch at composition time | No registry lookup at compose time | Pushes the dispatcher one layer up (every caller composes an `EncodeRequest`); `EncodeRequest` becomes codec-aware; cache key shape (ADR-0298) leaks the slice | Rejected — moves the problem rather than solving it |
| Generate the per-codec methods via codegen from a YAML table | DRY; tests + sources stay in lock-step | Adds a build-time codegen step the fork has otherwise avoided; no other adapter set in this tree uses codegen | Rejected for HP-1; revisit if the registry crosses ~30 adapters |

## Consequences

- **Positive**: 15 of 16 adapters become functional for live grid
  sweeps where they were silently broken (or crashing, in the libaom
  case). The codec-adapter contract is now enforced by the smoke test;
  drift between the docstring and the implementation is impossible.
- **Positive**: Phase D's per-shot ladder now drives any registered
  codec, not just libx264 — the historic hardcode in
  `per_shot._segment_command` was explicitly tagged as Phase A's
  scaffold limit (see file header).
- **Positive**: x264 and x265 argv stay byte-for-byte unchanged
  (asserted by the per-adapter smoke test) — no behaviour delta for
  the codecs that already worked.
- **Negative**: A test fixture table in
  `tests/test_encode_dispatcher_per_adapter.py` mirrors the
  registry; new adapters must add a row. The meta-test
  (`test_fixture_table_covers_every_registered_adapter`) makes this
  failure mode loud rather than silent.
- **Negative**: The libaom adapter's argv shape changed —
  callers that consumed the old `(-crf, str(crf), -cpu-used, ..., -an)`
  tuple verbatim need to update. Only `tests/test_codec_adapter_libaom.py`
  consumed the legacy shape and was updated in the same PR.
- **Neutral / follow-ups**: `parse_versions(stderr, encoder=...)` and
  the `run_encode(encoder_runner=...)` kwarg referenced by
  `tests/test_encode_multi_codec.py` are out of scope for HP-1; those
  tests stay red against this PR (eight remaining failures, all
  pre-existing on master).

## References

- [ADR-0237](0237-quality-aware-encode-automation.md): the original
  codec-adapter Protocol design.
- [ADR-0288](0288-vmaf-tune-codec-adapter-x265.md): libx265 adapter
  metadata (this PR adds its `ffmpeg_codec_args`).
- [ADR-0294](0294-vmaf-tune-codec-adapter-svtav1.md): libsvtav1 adapter
  metadata (this PR adds its `ffmpeg_codec_args`).
- [Research-0087](../research/0087-vmaftune-codec-adapter-dispatcher-hp1.md):
  HP-1 audit digest — the per-site argv-shape table that motivated the
  dispatcher pivot.
- Source: `req` ("Implementation task: HP-1 from Phase-A audit").
