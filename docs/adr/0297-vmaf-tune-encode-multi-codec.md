# ADR-0297: `vmaf-tune` — codec-agnostic encode dispatcher

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, ffmpeg, codec, automation, fork-local

## Context

ADR-0237 Phase A shipped `tools/vmaf-tune/` with the encode driver
(`encode.py`) hard-wired to `libx264`: the FFmpeg argv was literally
`-c:v libx264 -preset $PRESET -crf $CRF`, version parsing only knew
the `x264 - core <N>` line, and the corpus loop assumed CRF semantics
throughout. The codec-adapter registry existed in
`codec_adapters/__init__.py`, but the harness never asked the adapter
for anything — it only used `adapter.validate(...)` and
`adapter.encoder` for the row label.

Nine in-flight per-codec adapter PRs (libx265 #362, libsvtav1 #370,
libaom #360, libvvenc #368, NVENC #364, QSV #367, AMF #366,
VideoToolbox #373, plus subsequent waves bringing the total adapter
count to 17) all noted the same blocker: the adapter ships, but
`encode.py` cannot drive it end-to-end because the FFmpeg invocation
is x264-shaped. Each adapter PR either had to copy-and-mutate
`encode.py` (forking the harness 17 ways) or wait until the dispatcher
existed. The latter is one PR's worth of work that unblocks all 17;
the former is a permanent maintenance tax. ADR-0237's
codec-agnostic-search-loop invariant (rebase-notes #0227) explicitly
forbids the copy-and-mutate path.

## Decision

We will refactor `encode.py` into a thin codec-agnostic dispatcher.
`run_encode` looks up the adapter via
`codec_adapters.get_adapter(req.encoder)` and delegates argv composition
to two adapter methods:

- `adapter.ffmpeg_codec_args(preset, quality) -> list[str]` returns
  the `-c:v ...` argv slice, including the codec's quality knob
  (`-crf`, `-cq`, `-qp`, `-global_quality`, `-q:v`, ...) and any
  preset translation.
- `adapter.extra_params() -> tuple[str, ...]` returns optional
  non-codec argv (e.g. `-svtav1-params tune=0`, `-row-mt 1`).

Both methods are duck-typed via `getattr` with sane fallbacks: an
adapter without `ffmpeg_codec_args` falls back to the legacy
`-c:v <encoder> -preset <p> -crf <q>` shape, and a missing
`extra_params` is treated as empty. `parse_versions` gains an
`encoder=` keyword that picks a per-codec version probe from a small
table; unknown encoders return `("ffmpeg-version", "unknown")`.

The harness composes the final command as

```text
[ffmpeg, -y, -hide_banner, -loglevel info,
 -f rawvideo -pix_fmt <pf> -s WxH -r FR -i <src>,
 *adapter.ffmpeg_codec_args(preset, quality),
 *adapter.extra_params(),
 *req.extra_params,
 <output>]
```

`EncodeRequest` keeps its `crf` field for schema compatibility (the
JSONL row contract is frozen at SCHEMA_VERSION=1) and exposes a
`quality` property that mirrors `crf` so the codec-agnostic dispatcher
can talk to adapters in adapter-native vocabulary without bumping the
row schema.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Codec-agnostic dispatcher (chosen)** | Unblocks all 17 adapter PRs in one shot; preserves x264 path bit-identically; no schema bump; fallback path lets partial adapters keep working | Adapter contract is duck-typed, not a strict ABC | Picked: matches ADR-0237's "harness never branches on codec identity" invariant; one PR unlocks 17 |
| Per-codec `run_encode_<codec>` driver functions | Each codec ships with its own driver — simpler isolation per codec | Forks the harness 17 ways; the search loop, retry logic, and corpus row schema duplicate; rebase nightmare | Rejected: same problem ADR-0237 already ruled against |
| Strict `Protocol` with mandatory `ffmpeg_codec_args` | Type-checker catches missing methods | Forces every in-flight adapter PR to land the method before the dispatcher merges; the whole point is to unblock them | Rejected: hard rule says "DO NOT remove fallback for adapters that don't ship `ffmpeg_codec_args`" — keep duck-typed contract with fallback |
| Bump `SCHEMA_VERSION` to add a `quality` row key | Cleaner naming for non-CRF codecs | Forces every Phase B/C consumer to migrate; row schema is supposed to be backwards-compatible per ADR-0237 | Rejected: keep row at v1, expose `quality` only as a request-side property |
| Defer until each adapter PR lands and have the last one do the dispatcher | "Free" sequencing | Every adapter PR meanwhile copy-mutates `encode.py`; each subsequent rebase fights the others | Rejected: this is exactly what the user flagged as "needs to be done before any adapter PR" |

## Consequences

- **Positive**:
  - All 9 in-flight adapter PRs (NVENC #364, QSV #367, AMF #366,
    libaom #360, x265 #362, SVT-AV1 #370, VideoToolbox #373, VVenC
    #368, plus follow-on waves to 17) can now drive end-to-end
    encodes with no further changes to `encode.py`.
  - The harness invariant (codec-agnostic search loop, rebase-notes
    #0227) is now structurally enforced — there's nothing for an
    adapter author to fork.
  - The legacy x264 argv shape is bit-identical (existing 13-test
    suite still green; new 19-test multi-codec suite covers the
    dispatcher + 9 representative codec shapes + fallback).
- **Negative**:
  - Adapter contract is duck-typed (no `Protocol` enforcement at
    runtime). A forgotten `ffmpeg_codec_args` falls back silently to
    the x264 shape, which can mis-encode for non-x264 codecs. The
    new test suite catches this for every registered codec; the
    fallback exists specifically for the in-flight-PR window.
  - `parse_versions` regex table is hand-maintained. Each adapter
    PR is expected to extend it for its codec; tests pin the
    expected version-string format.
- **Neutral / follow-ups**:
  - Once every in-flight adapter PR lands, harden the contract with
    a runtime `hasattr` check + explicit warning for adapters
    missing `ffmpeg_codec_args` (deferred to a follow-up PR; the
    current fallback is intentionally permissive).
  - Phase B (target-VMAF bisect) consumes the same dispatcher
    unchanged — bisect logic stays codec-agnostic.

## References

- [ADR-0237](0237-quality-aware-encode-automation.md) — parent.
- [Research-0044](../research/0044-quality-aware-encode-automation.md) —
  option-space digest.
- [Research-0054](../research/0070-vmaf-tune-encode-multi-codec.md) —
  this PR's research digest.
- [`docs/usage/vmaf-tune.md`](../usage/vmaf-tune.md#codec-adapter-contract) —
  user-facing adapter contract docs.
- [`docs/rebase-notes.md`](../rebase-notes.md) — entry 0227 +
  this-PR entry pin the codec-agnostic-harness invariant.
- In-flight adapter PRs: #360 (libaom), #362 (libx265), #364 (NVENC),
  #366 (AMF), #367 (QSV), #368 (libvvenc), #370 (libsvtav1),
  #373 (VideoToolbox).
- Source: `req` — user requested "make encode.py codec-agnostic
  before any adapter PR can drive end-to-end encodes" (paraphrased
  from session 2026-05-03).
