<!-- markdownlint-disable MD060 -->
# Research-0065: NVENC codec adapters for `vmaf-tune` — option-space digest

- **Date**: 2026-05-03
- **Companion ADR**: [ADR-0290](../adr/0290-vmaf-tune-nvenc-adapters.md)
- **Status**: Snapshot at proposal time; the implementation PR
  supersedes the operational details.

## Question

Hardware encoders are 10–100× faster than software encoders and
produce a measurably different rate-distortion curve. How should
`vmaf-tune` model the NVIDIA NVENC family
(`h264_nvenc` / `hevc_nvenc` / `av1_nvenc`) inside the codec-adapter
contract established by [ADR-0237](../adr/0237-quality-aware-encode-automation.md)?
Specifically: one adapter per output codec or a single shared adapter
with a "use NVENC" flag on the existing `libx264` / `libx265` /
`libsvtav1` adapters?

## Findings

| Axis | NVENC | Software encoder family |
|---|---|---|
| Speed (1080p) | 200–800 fps (RTX 30/40) | 5–60 fps (libx264 medium) |
| VMAF at matched bitrate | typically 3–5 points lower | reference |
| Quality knob name | `-cq` (constant quantizer) | `-crf` |
| Quality knob range | 0..51 | 0..51 (libx264) |
| Preset count | 7 (`p1`..`p7`) | 9 (libx264 mnemonic names) |
| Preset names accepted | `p1`..`p7` only (or numeric) | `ultrafast`..`placebo` |
| Available everywhere? | NVIDIA GPU + driver | yes |
| AV1 availability | Ada Lovelace+ (RTX 40 / L40 / L4) | always |

The R-D characteristic is **distinct** at the curve level, not just
shifted — NVENC's `medium` and `slow` presets operate on
fundamentally different quality plateaus from the corresponding
libx264 mnemonics. A per-title or per-shot CRF predictor (Phase C / D
of ADR-0237) trained on libx264 data does not transfer to NVENC
without retraining.

## Decision matrix

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **A. One adapter per output codec** (`h264_nvenc`, `hevc_nvenc`, `av1_nvenc`) | Mirrors the ADR-0237 "one file per codec" principle; downstream Phase C predictor learns separate curves per codec name; no branching in the harness | Three near-identical files; needs a shared helper to avoid copy-paste | **Chosen** |
| B. One adapter per encoder family with a `hardware: bool` flag | Fewer files | Forces the harness to branch on the flag; muddies the registry's `name` → adapter-instance contract; codec one-hot in fr_regressor_v2 (six-bucket) doesn't naturally encode the hardware variant | Rejected — pushes codec-identity branching back into the search loop |
| C. Skip NVENC for now, ship after the software trio | Lower scope | Defers the user's actual ask; NVENC adapters are the smallest among the requested codec set and unblock corpus generation on GPU dev boxes immediately | Rejected |
| D. Wrap NVENC behind the libx264 adapter as an `--enable-nvenc` flag | Minimal new code | Cross-codec collision: `h264_nvenc` and `libx264` produce different output codecs (still both H.264 streams, but different RD characteristics); muddles the corpus row's `encoder` column | Rejected |

## Mnemonic preset mapping rationale

NVENC has 7 hardware preset levels; libx264 has 9 named ones. We map
the 10 mnemonic names (libx264's 9 + `slowest` as an explicit alias)
onto `p1`..`p7` per the table in `_nvenc_common.py`. The choice
clamps the fast end at `p1` (so that `ultrafast`/`superfast`/
`veryfast` all map there) and the slow end at `p7` (so that
`slowest`/`placebo` both map there); the middle six mnemonics map
1:1. This keeps cross-codec sweeps consistent — a sweep over
`(medium, slow)` produces comparable preset semantics across libx264,
NVENC, x265, and svtav1.

## Codec one-hot expansion (follow-up)

`fr_regressor_v2` currently uses a 6-slot codec one-hot. Adding
NVENC's three codecs pushes the natural slot count to ≥ 9 (and
≥ 12 if the parallel x265 / svtav1 / libaom adapter PRs land
together). The one-hot expansion is a separate follow-up — the
adapter contract is the unblocker; the model schema bump is gated on
training corpus availability.

## References

- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) — parent surface.
- FFmpeg NVENC docs: <https://trac.ffmpeg.org/wiki/HWAccelIntro#NVENC>.
- NVIDIA Video Codec SDK preset table: <https://docs.nvidia.com/video-technologies/video-codec-sdk/12.0/nvenc-preset-migration-guide/index.html>.
- Source: `req` (user direction, 2026-05-03 task brief).
