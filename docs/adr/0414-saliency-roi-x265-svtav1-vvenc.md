<!-- markdownlint-disable MD060 -->
# ADR-0414: Saliency-aware ROI for x265 / SVT-AV1 / libvvenc adapters

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vmaf-tune`, `saliency`, `codec-adapter`, `roi`, `fork-local`

## Context

The fork's x264 adapter already delivers per-frame QP-offset maps via the
`saliency.py` pipeline (DUTS-trained U-Net, `saliency_student_v1.onnx`).
x264 consumes the map through its ASCII `--qpfile` mechanism (ADR-0293).
The three other software encoders in the Phase A adapter set — libx265,
libsvtav1, and libvvenc — each expose a distinct ROI mechanism:

- **libx265**: `--zones=startfrm,endfrm,q=<delta>` passed through
  `-x265-params zones=…` (the same KV channel the two-pass `pass=N` flag uses).
- **SVT-AV1**: `--qp-file` (v1.7+) accepts a plain-text file of space-separated
  QP delta rows at 64×64 super-block granularity, passed through
  `-svtav1-params qp-file=…`.
- **libvvenc**: `--roi` / `ROIFile` accepts a CSV of comma-separated QP delta
  rows at 64×64 CTU granularity, passed through `-vvenc-params ROIFile=…`.

PR #456 attempted to add this support but was closed because it predated the
F.3/F.5/conformal-intervals surface and conflicted with `--two-pass` /
`--with-uncertainty` in the CLI. This ADR re-ports the feature cleanly on top
of current master without touching the conformal surface.

## Decision

We will extend `vmaftune.saliency` with three new pure-function formatters:

- `write_x265_zones_arg` — reduces the per-block offset map to a single spatial
  mean and emits an x265 `--zones` string covering the full clip duration.
- `write_svtav1_qpoffset_map` — writes a space-separated QP-offset file at
  64×64 super-block granularity (one frame per frame, blank-line separator).
- `write_vvenc_roi_csv` — writes a comma-separated ROI-delta CSV at 64×64 CTU
  granularity (same frame layout, comma delimiter instead of space).

Corresponding `augment_extra_params_with_*` helpers follow the same pattern as
the existing x264 helper so callers stay symmetric.

Each adapter gains a `supports_saliency_roi: bool = True` field and a
delegating method (`zones_from_saliency`, `qpmap_from_saliency`,
`roi_from_saliency`) that wires through to the formatter. The x265 adapter's
existing `supports_two_pass: bool = True` and `supports_qpfile: bool = False`
fields are unchanged — zones-based saliency ROI is orthogonal to 2-pass.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| x264 qpfile format for all three codecs | Simpler — one formatter | libx265 / SVT-AV1 / vvenc do not honour x264's ASCII qpfile syntax | Rejected: wrong format per encoder docs |
| Per-MB (16×16) block granularity for SVT-AV1 / vvenc | Matches x264 MB granularity | SVT-AV1 super-block is 64×64; vvenc CTU is 64×64; sub-SB granularity is undefined | Rejected: encoders document 64×64 as the ROI-map unit |
| Single-zone x265 approach (mean offset, all frames) | Simple; matches per-clip aggregate saliency mask semantics | Loses within-clip spatial variation | Chosen: per-clip aggregate is the established posture (ADR-0293); temporal zones are a follow-up |
| Reuse `saliency_aware_encode` to dispatch per-codec | One call site | Would require branching on encoder identity inside the helper | Rejected: ADR-0294 forbids codec-identity branches in the encode driver |

## Consequences

- **Positive**: x265, SVT-AV1, and VVenC encodes can consume the DUTS-trained
  saliency map via their native ROI channel with no behaviour change to
  existing x264 / two-pass / conformal-intervals paths.
- **Negative**: x265 zones deliver a single spatial-mean delta per clip
  (not true per-block granularity) because x265's `--zones` has temporal
  granularity only. Per-block spatial fidelity requires a future x265 qpfile
  port.
- **Neutral / follow-ups**:
  - The SVT-AV1 qpmap granularity is 64×64; the saliency map is typically
    720p/1080p, so the block grid can be small (e.g. 12×17 at 720p). This is
    expected and accepted.
  - Temporal per-zone x265 support (multiple `startfrm,endfrm` windows) is
    deferred to a follow-up PR.
  - `write_svtav1_qpoffset_map` and `write_vvenc_roi_csv` replicate the same
    spatial mask across all frames because the saliency pipeline produces a
    per-clip aggregate. Frame-level temporal saliency is a separate effort.

## References

- ADR-0293 (x264 saliency-aware ROI — parent design)
- ADR-0286 (`saliency_student_v1.onnx` model)
- ADR-0247 (`vmaf-roi` QP-offset convention)
- ADR-0333 (Phase F two-pass — unchanged by this ADR)
- SVT-AV1 `Source/App/EncApp/EbAppConfig.c` `read_qp_map_file`, tag `v2.1.0`
- VVenC `VVEncAppCfg.h` `ROI_FILE` key, tag `v1.14.0` (SHA `9428ea86`)
- libx265 encoder options table — `zones` key, x265 docs 3.x
- Re-port of closed PR #456 (design review comment cited)
