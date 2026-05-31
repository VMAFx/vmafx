<!-- markdownlint-disable MD060 -->
# ADR-0247: vmaf-roi sidecar binary for per-CTU QP offsets

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: lusoris, Claude
- **Tags**: `tools`, `ai`, `roi`, `encoder`

## Context

T6-2a (ADR-0218 / PR #208) shipped the in-libvmaf `mobilesal` saliency
extractor — same model, scoring side: it tells callers where the model
expects perceptual error. T6-2b is the encoder-steering counterpart: a
CLI sidecar that consumes the same saliency map and emits a per-CTU
QP-offset file the encoder reads back. Two surfaces, one model.

The fork already ships several CLI tools under `core/tools/` (`vmaf`,
`vmaf_bench`); a new sidecar binary slots into that pattern with no
public C-API impact and no library link surface beyond the existing
`libvmaf/dnn.h` session API.

The decision space sits at the intersection of three orthogonal axes:
the sidecar **format** (ASCII vs binary, encoder-specific), the
**reduction** (mean vs max over CTU samples), and the **encoder
coverage** (x265-only vs multi-encoder). Each pick locks in a contract
that downstream encoder drivers will depend on.

## Decision

We ship `vmaf-roi` as a fork-local sidecar binary at
`core/tools/vmaf_roi.c` that:

1. Consumes raw planar YUV input + a 0-based frame index, seeks to the
   requested frame with `fseeko` (>2 GiB safe), reads only the luma
   plane.
2. Computes a per-pixel saliency map via the optional `--saliency-model`
   ONNX session (`vmaf_dnn_session_run_luma8`), or via a deterministic
   center-weighted radial placeholder when no model is provided
   (smoke-test fallback only — explicitly documented as not for real
   encodes).
3. Reduces the per-pixel map to a per-CTU **mean** (not max) over each
   CTU's bounding box, with partial CTUs at right/bottom edges
   averaged over their actual sample count.
4. Maps `[0, 1]` saliency to a signed QP offset via
   `qp = clamp(-strength * (2 * saliency - 1), -12, +12)` — high
   saliency drives the offset negative (boost quality), low saliency
   positive (save bits), neutral (~0.5) zero.
5. Emits **two formats** selected by `--encoder`: ASCII grid for x265
   (`--qpfile-style`, with two `#` comment header lines documenting
   frame / CTU / strength) and raw `int8_t` binary for SVT-AV1
   (`--roi-map-file`, no header).
6. Operates **one frame per invocation**; multi-frame batching is a
   shell driver loop (or a future built-in mode).

## Alternatives considered

| Axis | Option | Pros | Cons | Why not chosen |
|---|---|---|---|---|
| Format | **ASCII per-row grid (chosen for x265)** | Human-readable; matches x265's `--qpfile-style` precedent; trivial to diff in CI | ~2 - 3x larger on disk than binary; slower to parse for very large grids | Selected for x265 — the encoder's own qpfile-style is ASCII, so we follow that convention rather than fight it. |
| Format | **Raw int8 binary (chosen for SVT-AV1)** | Compact (1 byte per CTU); matches SVT-AV1's `--roi-map-file` byte layout | Not human-readable; needs a hex-dump tool to inspect | Selected for SVT-AV1 — the encoder explicitly requires this layout, no choice. |
| Format | Single universal format (e.g. JSON) | Encoder-agnostic on disk | Every encoder driver still needs a converter; defeats the purpose of "sidecar" | Rejected: it just moves the conversion cost to the consumer. |
| Reduction | **Per-CTU mean (chosen)** | Matches what most ROI heuristics use; smooth; partial-CTU clamping is straightforward | A single salient pixel inside a mostly-flat CTU gets averaged out | Selected: faces / focal subjects fill many pixels, so the mean tracks the perceptual signal well in practice. |
| Reduction | Per-CTU max | One-pixel anomalies still bias the offset; defends against under-allocating to small but important regions | Wildly oversensitive to MobileSal noise; many CTUs end up at the +12/-12 clamp | Rejected: noise on a learned saliency map is the dominant failure mode, not under-coverage. |
| Reduction | Per-CTU 90th percentile | Compromise between mean and max | Adds a per-CTU sort; doesn't measurably outperform mean on Wave 1 sweeps | Deferred: revisit if mean shows under-allocation in real encodes. |
| Encoder coverage | **x265 + SVT-AV1 day-one (chosen)** | Covers the two encoders most users pair with libvmaf; one binary handles both | Slightly larger code surface; two emit paths to maintain | Selected: the cost of adding the second emit path is ~30 LoC; gating SVT-AV1 to a follow-up PR doubles review rounds for no real savings. |
| Encoder coverage | x265-only first, SVT-AV1 later | Smallest possible PR | Forces a follow-up PR + ADR for what is fundamentally one decision | Rejected: same review cost twice. |
| Signal blend | **Saliency-only (chosen)** | One signal, one model, one place to invest | A flat / textureless salient region still gets a strong negative offset even though encoders need fewer bits there | Selected for T6-2b: keep the contract simple. |
| Signal blend | Saliency × edge density | Better per-CTU fidelity; punishes flat regions appropriately | Needs a second pass (Sobel / gradient) per frame; couples the sidecar to the libvmaf feature graph | Deferred: tracked as a Wave 2 follow-up; the `vmaf-roi` CLI surface is forward-compatible (a `--blend edge-density` flag drops in cleanly). |

## Consequences

- **Positive**: encoder-side ROI now has a documented, lint-clean,
  test-covered fork tool that any encoder driver can shell out to.
  Same `mobilesal` ONNX feeds both scoring (T6-2a) and steering
  (T6-2b); no model duplication.
- **Positive**: 8-bit-only contract is explicit in `--bitdepth` (only
  `8` accepted) and in `docs/usage/vmaf-roi.md`; 10/12-bit lands when
  the `mobilesal` extractor's bit-depth contract is finalised, not
  before.
- **Negative**: per-CTU mean is a known signal-attenuation point; we
  accept it for Wave 1 and revisit if real encodes show
  under-allocation. The CLI is forward-compatible with a `--blend`
  flag for the edge-density follow-up.
- **Neutral / follow-ups**:
  - Wave 2 should add multi-frame batch mode (one input, N frames per
    invocation) so encoder drivers don't pay the ORT-session cost N
    times. Tracked in the roadmap as a sub-bullet of T6-2b.
  - SVT-AV1's `--roi-map-file` reads one map per frame; the per-frame
    file naming convention used in `docs/usage/vmaf-roi.md` is the
    consumer's responsibility for now.

## References

- T6-2b roadmap entry: `docs/ai/roadmap.md` § Wave 1 saliency surface.
- ADR-0218 / PR #208 — `mobilesal` saliency extractor (T6-2a, the
  scoring-side counterpart).
- ADR-0042 — tiny-AI per-PR docs rule (each AI surface ships docs in
  the same PR).
- ADR-0100 — project-wide doc-substance rule.
- ADR-0108 — six deep-dive deliverables.
- ADR-0141 — touched-file lint-clean rule (the refactor of
  `parse_args` / `main` / `vmaf_roi_reduce_per_ctu` was driven by
  this).
- Source: roadmap T6-2b, Wave 1 saliency surface (planning dossier
  `.workingdir2/`).
