<!-- markdownlint-disable MD013 MD060 -->
# ADR-0283: `vmaf-tune` Apple VideoToolbox codec adapters

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: lusoris (with Claude)
- **Tags**: tooling, ai, ffmpeg, codec, hardware-encoder, apple, fork-local

## Context

`tools/vmaf-tune/` (the quality-aware encode automation harness from
[ADR-0237](0237-quality-aware-encode-automation.md)) has accumulated
adapters for the NVIDIA NVENC, AMD AMF, and Intel QSV hardware
families along the same contract. Apple VideoToolbox is the missing
HW path on macOS — needed for the per-title CRF predictor's coverage
on Apple Silicon hosts and for any future codec-aware regressor
retrain that wants to learn the VT distortion signature.

Apple's VideoToolbox is the only hardware path on M-series and on
Intel Macs with a T2 chip. FFmpeg exposes two encoders backed by it
(`h264_videotoolbox`, `hevc_videotoolbox`); AV1 hardware encoding
is not available on Apple Silicon as of 2026 and is intentionally
omitted from this PR.

The harness's codec-adapter contract
(`tools/vmaf-tune/src/vmaftune/codec_adapters/__init__.py`) is
multi-codec from day one (per
[`tools/vmaf-tune/AGENTS.md`](../../tools/vmaf-tune/AGENTS.md)) — new
codecs are one-file additions under `codec_adapters/` and the search
loop never branches on codec identity. This PR adds the two
VideoToolbox adapters in line with that contract.

## Decision

Add `H264VideoToolboxAdapter` and `HEVCVideoToolboxAdapter` under
`tools/vmaf-tune/src/vmaftune/codec_adapters/`, sharing a single
`_videotoolbox_common.py` for the quality-knob and preset mapping.
Both adapters carry `invert_quality=False` and a `[0, 100]` quality
range — the harness's `crf` row slot now carries whatever native
quality value each adapter declares, with downstream consumers
interpreting the knob via the adapter registry.

The nine-name preset taxonomy maps onto VT's coarser `-realtime` flag:
`ultrafast`/`superfast`/`veryfast`/`faster`/`fast` → `-realtime 1`;
`medium`/`slow`/`slower`/`veryslow` → `-realtime 0`. The mapping is
intentionally lossy — VT cannot expose a finer dial.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Two adapters sharing `_videotoolbox_common.py` (chosen) | Codec-specific files stay tiny (~30 LOC each); shared preset/quality logic deduplicated; matches the per-codec one-file-per-adapter convention from AGENTS.md. | One extra module file. | Best fit for the codec-adapter contract. |
| Single `videotoolbox.py` with two classes | One file. | Mixes h264 + hevc encoder names in one module; breaks the "one file per codec" convention; harder to grep for. | Convention drift. |
| Inline `-realtime` mapping inside each adapter | No shared file. | Duplicated preset → realtime logic between two adapters; preset list copy-pasted. | DRY violation. |
| Map presets to a synthetic `-prio_speed`/`-allow_sw` matrix | More expressive than `-realtime`. | VideoToolbox does not expose those as encoder options uniformly across macOS versions; brittle. | Out-of-band knobs, not stable. |
| Skip VideoToolbox until a macOS CI runner exists | Avoid adding code that can't be exercised on Linux CI. | Blocks `fr_regressor_v2_hw`'s hardware-codec coverage; macOS contributors can already exercise locally. | Test-mockable; subprocess boundary is the seam. |

## Consequences

- **Positive**: a future codec-aware regressor retrain can include
  Apple-Silicon-encoded distortions in its corpus once the harness
  runs on a macOS host; the codec-adapter contract is exercised by
  one more live adapter family (proves the registry pattern). Tests
  mock `subprocess.run` so the suite stays Linux-CI-runnable.
- **Negative**: VT's `-realtime` is a coarse preset axis; the harness
  cannot drive a finer speed/quality dial on Apple hardware. The
  `-q:v` quality scale (0..100, higher = better) differs from CRF's
  scale (0..51, lower = better) so corpus consumers must read
  `encoder` + `crf` together via the adapter registry — documented
  in [`docs/usage/vmaf-tune.md`](../usage/vmaf-tune.md).
- **Neutral / follow-ups**: a real macOS CI runner is out of scope
  here; the smoke gate runs on Linux against mocked subprocess. AV1
  hardware encoding will land when `av1_videotoolbox` ships in
  FFmpeg + a supported macOS version.

## References

- [ADR-0237](0237-quality-aware-encode-automation.md) — `vmaf-tune` umbrella spec.
- [`tools/vmaf-tune/AGENTS.md`](../../tools/vmaf-tune/AGENTS.md) — codec-adapter contract invariants.
- [`docs/usage/vmaf-tune.md`](../usage/vmaf-tune.md) — user-facing CLI surface.
- Source: `req` (paraphrased) — user requested Apple VideoToolbox encoder adapters; the originally-coupled codec-vocab schema expansion is split into a separate follow-up PR awaiting a fresh production retrain (per ADR-0235 + ADR-0291 ship-gate).

## Status update 2026-05-09: ProRes adapter added

The macOS hardware-encoder coverage trio is completed by adding
`prores_videotoolbox` alongside the original `h264_videotoolbox` and
`hevc_videotoolbox` adapters. The body of this ADR remains frozen
per the "Immutable once Accepted" rule
([`docs/adr/README.md`](README.md) §Conventions); this appendix
records the follow-on that landed in the same registry pattern
without re-opening the original decision.

The ProRes adapter is deliberately a sibling of the H.264 / HEVC VT
adapters — same registry shape, same `_videotoolbox_common.py`, same
nine-name preset → `-realtime` mapping. The one shape difference
is the **quality knob**:

| Adapter | Knob | Range | Direction |
| --- | --- | --- | --- |
| `h264_videotoolbox` / `hevc_videotoolbox` | `q:v` | 0..100 | higher = better |
| `prores_videotoolbox` | `profile:v` | 0..5 | higher tier = better |

ProRes is a fixed-rate intermediate codec — it has no CRF / QP /
`q:v` style scalar. The harness's `crf` slot carries the integer
ProRes tier id (0=`proxy`, 1=`lt`, 2=`standard` (422), 3=`hq` (422
HQ), 4=`4444`, 5=`xq` (4444 XQ)) per the FFmpeg
`prores_videotoolbox` `profile` AVOption table (verified against
`libavcodec/videotoolboxenc.c` `prores_options` in FFmpeg n8.1.1).
The corpus row's existing `encoder` + `crf` pair carries the tier
choice; downstream consumers translate the integer back via the
adapter registry, so no schema change is needed.

Hardware availability: M1 Pro / Max / Ultra and every later
M-series chip ship the ProRes hardware block; Intel Macs with a T2
chip do **not** have it (FFmpeg falls back to the software
`prores_aw` / `prores_ks` encoders there).

`ENCODER_VOCAB` invariant: ProRes is **not** in the live
`ENCODER_VOCAB_V2` (12 slots, frozen by [ADR-0291](0291-fr-regressor-v2-prod-ship.md))
nor in the `ENCODER_VOCAB_V3` scaffold (16 slots, scaffold-only by
[ADR-0302](0302-encoder-vocab-v3-schema-expansion.md)). The
`prores_videotoolbox` slot will land as part of a future v4 vocab
bump tracked under T-FR-V2-VOCAB-V3-RETRAIN — until then, the
proxy-fast-path raises `ProxyError` on ProRes input and the harness
falls back to the live-encode loop, which works unchanged.

Reference: [`tools/vmaf-tune/src/vmaftune/codec_adapters/prores_videotoolbox.py`](../../tools/vmaf-tune/src/vmaftune/codec_adapters/prores_videotoolbox.py).
