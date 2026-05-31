<!-- markdownlint-disable MD060 -->
# ADR-0290: NVENC codec adapters for `vmaf-tune` (h264 / hevc / av1)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, codec, nvenc, gpu, fork-local

## Context

`vmaf-tune` Phase A
([ADR-0237](0237-quality-aware-encode-automation.md)) shipped with a
single `libx264` adapter. The codec-adapter contract was designed
multi-codec from day one — every codec lives in its own file and the
harness search loop never branches on codec identity. The next batch
of codecs to wire in is the NVIDIA NVENC family (`h264_nvenc`,
`hevc_nvenc`, `av1_nvenc`): hardware encoders that are 10–100×
faster than the software trio and operate on a measurably different
rate-distortion curve. They cannot be modelled as a flag on the
software adapters because their R-D characteristics, default-quality
plateaus, and Phase C / D predictor curves are codec-distinct, not
codec-shifted.

## Decision

We will ship one codec adapter per NVENC output codec —
`H264NvencAdapter`, `HevcNvencAdapter`, `Av1NvencAdapter` — under
`tools/vmaf-tune/src/vmaftune/codec_adapters/` mirroring the existing
`X264Adapter` shape. Common preset / CQ vocabulary lives in a private
`_nvenc_common.py` helper (mnemonic → `p1`..`p7` map, CQ hard
limits `[0, 51]`, Phase A informative window `[15, 40]`). The
mnemonic preset names accepted by the libx264 adapter are reused
verbatim so cross-codec sweeps over `(medium, slow) × (22, 28, 34)`
remain semantically aligned.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One adapter per output codec (chosen) | Matches ADR-0237 "one file per codec"; downstream codec one-hot remains unambiguous; no harness branching | Three near-identical files; needs a shared helper | — |
| One shared adapter family with `hardware: bool` flag | Fewer files | Forces codec-identity branching back into the search loop; collides with the codec one-hot in `fr_regressor_v2` | Pushes complexity into exactly the place ADR-0237 said it must not go |
| Defer NVENC to a later phase | Lower scope | Blocks GPU-host corpus generation and is the smallest of the requested codecs | Defers a low-cost unblocker |
| Wrap NVENC under the libx264 adapter as `--enable-nvenc` | Minimal new code | `libx264` and `h264_nvenc` produce different RD curves at the same-named CQ/CRF — mixing them under one adapter row corrupts the corpus | Cross-codec contamination |

## Consequences

- **Positive**: Three new codecs land for the same scaffolding cost as
  a single one (shared `_nvenc_common.py`). Corpus generation on
  GPU-equipped dev boxes becomes 10–100× faster. The codec-adapter
  contract is now exercised by both software (libx264) and hardware
  (NVENC) implementations, validating that the harness genuinely
  doesn't branch on codec identity.
- **Negative**: `fr_regressor_v2`'s 6-slot codec one-hot is no longer
  large enough — adding NVENC's three codecs (and the parallel
  software-codec PRs for x265 / svtav1 / libaom) pushes the natural
  count to ≥ 9, with headroom for ≥ 12 once QSV / VAAPI follow.
  The one-hot expansion is a separate follow-up, gated on training
  corpus availability for the new codecs.
- **Neutral / follow-ups**:
  - Schema-expansion follow-up filed for `fr_regressor_v2` codec
    one-hot (≥ 12 slots before training v2 on a corpus that
    includes hardware codecs).
  - The corpus row's `encoder` column already records the FFmpeg
    encoder name verbatim, so existing JSONL consumers see
    `"encoder": "h264_nvenc"` distinct from `"encoder": "libx264"`
    without schema changes.
  - The harness encode driver (`encode.py`) currently emits `-crf`
    unconditionally. NVENC accepts `-crf` as an alias for `-cq` in
    recent FFmpeg versions, so existing argv emission works; the
    codec adapter exposes `quality_knob = "cq"` for a follow-up that
    teaches `build_ffmpeg_command` to emit the codec-correct flag.

## References

- [ADR-0237](0237-quality-aware-encode-automation.md) — parent
  `vmaf-tune` umbrella spec.
- Companion research digest:
  [`docs/research/0065-vmaf-tune-nvenc-adapters.md`](../research/0065-vmaf-tune-nvenc-adapters.md).
- FFmpeg NVENC: <https://trac.ffmpeg.org/wiki/HWAccelIntro#NVENC>.
- NVIDIA Video Codec SDK preset table:
  <https://docs.nvidia.com/video-technologies/video-codec-sdk/12.0/nvenc-preset-migration-guide/index.html>.
- Source: `req` (user direction, 2026-05-03 task brief).
