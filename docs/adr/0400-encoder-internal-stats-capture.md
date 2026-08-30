<!-- markdownlint-disable MD060 -->
# ADR-0400: encoder-internal-stats capture (corpus expansion v1)

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: lusoris, Claude
- **Tags**: vmaf-tune, corpus, predictor, x264

## Context

The `vmaf-tune` corpus generator currently records only the post-hoc
encode outcome — output size, wall time, VMAF score, version
provenance. The encoder's own rate-distortion ledger (predicted
bitrate, QP, motion-vector cost, texture cost, partition decisions)
is discarded. Per the contributor-pack research digest
(`docs/research/0086-contributor-pack-web-data-expansion-2026-05-08.md`,
ranked highest-leverage signal expansion), this signal is "free at
encode time" — already produced by every modern encoder during a
single `--pass 1` / `-pass 1 -passlogfile <prefix>` invocation.
Capturing it closes the loop on what the encoder's *own*
rate-distortion engine saw, not just on what the input pixels look
like.

## Decision

We extend the corpus row schema (bumped to v3) with ten
``enc_internal_*`` scalar aggregates capturing per-frame x264 pass-1
stats: mean/std QP, mean/std bits, mean/std MV cost, mean intra-texture
cost, mean predicted-texture cost, intra-MB ratio, skip-MB ratio. The
codec-adapter contract gains a ``supports_encoder_stats: bool`` flag;
libx264 and libx265 opt in (libx264 parsed in v1, libx265 wired
through but parser in a follow-up), every hardware encoder and
software AV1 encoder opts out. Rows for opt-out codecs emit zeros so
the schema is uniform across the corpus.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Capture stats inline during the production encode | One pass instead of two | x264's stats writer requires `-pass 1` mode; mode-switching mid-encode isn't supported by FFmpeg | Two-pass (stats-only pass + production CRF pass) is the canonical workflow |
| Parse the post-encode bitstream with `ffprobe -show_frames` | No second encode | Misses RC-internal signal (QP-pre-AQ, partition cost, predicted bits); ffprobe is decoder-side, not encoder-side | Can't recover the "what the encoder considered" signal |
| Defer until a multi-codec parser is ready (libx265, libvpx) | Single PR delivers all codecs | Predictor-integration follow-up is gated on x264 alone landing; multi-codec adds weeks | Ship x264 now; libx265/libvpx parsers land additively |

## Consequences

- **Positive**: predictor gains the encoder's own RD-loop signal, which
  the contributor-pack digest sized as the highest-leverage available
  feature. Schema is uniform across codecs (zeros for opt-out).
  Adapter contract is explicit — adding a new encoder forces an
  opt-in/opt-out choice.
- **Negative**: per-encode wall-clock cost roughly doubles for
  ``supports_encoder_stats=True`` adapters — we run a stats-only
  pass-1 invocation before the production CRF encode. The corpus
  sweep gains the new columns at 2× cost. This is the documented
  trade-off; no way to recover the signal without the extra pass.
- **Neutral / follow-ups**: predictor integration is **out of scope**
  for this PR; the predictor consumes the new columns in a follow-up
  after the schema lands. Tests verify the columns are populated and
  schema-uniform; correlation with VMAF is measured downstream.
  libx265 / libvpx-specific parsers land in additional follow-up PRs
  on top of the v3 schema (no further version bump needed).

## References

- Research digest:
  `docs/research/0086-contributor-pack-web-data-expansion-2026-05-08.md`
  (ranked encoder-internal stats as highest-leverage signal expansion).
- x264 source: `encoder/ratecontrol.c`,
  `x264_ratecontrol_summary` writer.
- FFmpeg pass-1: `doc/encoders.texi`, `libavcodec/libx264.c` (passlog
  passthrough).
- Coordinates with ADR-0302 (encoder-vocab-v3-schema-expansion);
  whichever PR lands first claims SCHEMA_VERSION=3, the other lands
  its columns additively.
- Source: `req` (direct user request: "capture per-frame
  encoder-internal statistics that x264 already emits via `--pass 1`
  / `--stats`").
