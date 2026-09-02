<!-- markdownlint-disable MD060 -->
# ADR-0279: vmaf-tune codec adapter — libaom-av1

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tools, vmaf-tune, av1, codec-adapter

## Context

`vmaf-tune` ships a quality-aware encode automation harness
([ADR-0237](0237-quality-aware-encode-automation.md)) whose Phase A
scaffolds a `libx264` grid sweep. The downstream phases — target-VMAF
bisect (B), per-title CRF predictor (C), per-shot dynamic CRF (D),
Pareto ABR ladder (E) — are codec-agnostic, but only run interesting
sweeps when the adapter registry covers more than one encoder.

The fork already has companion PRs adding `libx265` and `libsvtav1`
adapters in parallel. This ADR records the decision to add a
`libaom-av1` adapter as the fourth codec slot in the
`fr_regressor_v2` six-slot codec one-hot (`x264, x265, svtav1, libaom,
?, ?`). libaom (Google's reference AV1 encoder) sits at a different
operating point on the AV1 speed/quality curve than SVT-AV1: it is
meaningfully slower at matched preset names but tends to deliver
slightly higher quality at the same bitrate at slow presets per
published AOM benchmarks. Covering both encoders lets Phase C/D
predictors compare AV1 implementations on identical sources.

## Decision

We will add a `LibaomAdapter` under
`tools/vmaf-tune/src/vmaftune/codec_adapters/libaom.py` exposing the
canonical adapter contract (name, encoder, quality knob, range,
default, invert flag, presets tuple, validation) plus two
codec-specific helpers: `cpu_used(preset) -> int` mapping the
human-readable preset vocabulary onto libaom's `-cpu-used` integer
0..9, and `ffmpeg_codec_args(preset, crf) -> tuple[str, ...]`
returning the argv slice that goes after `-c:v libaom-av1`. The
quality range is the full libaom CRF window `[0, 63]`. The preset
vocabulary parallels x264/x265 (`placebo, slowest, slower, slow,
medium, fast, faster, veryfast, superfast, ultrafast`) so a single
sweep axis covers all four codecs without branching on codec name in
the search loop.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Skip libaom; rely on SVT-AV1 alone for AV1 coverage | One adapter to maintain; SVT-AV1 is faster and the obvious "default" AV1 encoder | Loses the high-quality archive operating point; predictors can't pick the better encoder per-source | Half the AV1 design space is invisible to the corpus |
| Expose `cpu-used` as the integer knob directly (no preset names) | One-to-one with libaom's CLI; no mapping table to maintain | Search loop's preset axis becomes per-codec, breaking the "single sweep covers all codecs" property | Defeats the codec-adapter contract |
| Wire libaom's full encoder argv (`-row-mt`, `-tile-columns`, `-aq-mode`, ...) inline now | Single landing for all libaom expressivity | Out of scope for an adapter scaffold; couples Phase A landing to argument-design churn | Phase B+ owns the wider argv surface; adapter exposes the metadata only |

## Consequences

- **Positive**: AV1 corpus rows now span both AV1 implementations.
  Phase C/D regressors get four codecs of training signal under the
  same preset vocabulary. The fr_regressor_v2 six-slot codec one-hot
  is half-populated.
- **Negative**: libaom is meaningfully slower than SVT-AV1 at matched
  presets; sweeps that include it cost more wall time. Documented
  trade-offs on the adapter doc page so users pick the encoder
  intentionally.
- **Neutral / follow-ups**: `vmaftune.encode.build_ffmpeg_command`
  still hardcodes the `-preset` / `-crf` argv shape. Routing the
  libaom adapter's `ffmpeg_codec_args(...)` slice through that
  builder is a follow-up that lands when the codec-pluggable encode
  path lands (one path for x265's `-preset` family, another for
  libaom's `-cpu-used` family).

## References

- [ADR-0237](0237-quality-aware-encode-automation.md) — vmaf-tune
  umbrella spec.
- [`tools/vmaf-tune/AGENTS.md`](../../tools/vmaf-tune/AGENTS.md) —
  adapter contract invariants.
- [`docs/usage/vmaf-tune.md`](../usage/vmaf-tune.md) — user-facing
  adapter documentation.
- libaom AV1 encoder upstream:
  <https://aomedia.googlesource.com/aom/>.
- FFmpeg `libaom-av1` encoder docs:
  <https://ffmpeg.org/ffmpeg-codecs.html#libaom_002dav1>.
- Source: `req` — paraphrased from session direction: ship a libaom
  codec adapter as the AV1 reference-encoder companion to the
  parallel SVT-AV1 adapter; preset names map onto `cpu-used` 0..9;
  document the libaom-vs-SVT-AV1 trade-off (libaom slower but
  slightly higher quality at slow presets per AOM benchmarks).
