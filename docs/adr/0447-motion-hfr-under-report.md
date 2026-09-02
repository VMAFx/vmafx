<!-- markdownlint-disable MD013 MD060 -->
# ADR-0447: Motion features under-report on HFR / 50p content

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris, lawrencecurtis
- **Tags**: `ai`, `motion`, `hfr`, `feature-extractor`, `cuda`, `sycl`, `vulkan`, `fork-local`

## Context

VMAF motion features (motion v1, v2, v3 and their GPU twins) compute a
frame-to-frame luminance delta without compensating for the source frame rate.
At 50 p or any HFR source above 30 fps the per-frame delta is smaller because
each frame is temporally closer to its neighbour; the resulting motion score
therefore under-reports motion energy relative to the same content captured at
30 fps.

The downstream consequence is a systematic VMAF bias on HFR content: the model
interprets a low motion score as a "smoother" clip and predicts higher quality,
even though the source can be as motion-busy as — or busier than — an equivalent
30 fps clip.

The gap was identified during the CHUG extraction run (Issue #837, reported by
lawrencecurtis via a BBC 50p test sequence). The same audit also uncovered a
parallel CAMBI/HDR EOTF calibration gap (CAMBI invoked without `eotf=pq` on
PQ HDR content, causing under-detection of visible banding on the 568+ HDR
clips in the in-flight extraction); that sibling pattern is documented alongside
the HFR fix and addressed in the same code change (PR #851), but may warrant a
separate ADR if the CAMBI gap receives its own dedicated decision record.

## Decision

We will apply a `motion_fps_weight = clamp(30 / fps, 0.25, 4.0)` multiplier to
the motion score across all motion extractor variants (CPU, CUDA, SYCL, Vulkan).
The source frame rate is detected from the per-clip sidecar (the CHUG/K150K
parquet metadata enriched by ADR-0434) or from an `ffprobe` call when no
sidecar is available. The weight is clamped to `[0.25, 4.0]` to bound the
correction on extreme frame rates (8 fps and 120 fps respectively) and to keep
the fix conservative for the common 30 fps baseline (weight = 1.0, no-op).

The cross-backend ULP parity gate (ADR-0214) must include the
`motion_fps_weight` computation path so that SIMD-path drift in the clamping
arithmetic is caught by CI.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Re-extract on a 30 fps downsampled copy | Numerically equivalent to the 30 fps assumption; no model change needed | Doubles I/O and decode cost; re-encode introduces its own temporal artefacts | Cost unacceptable for in-flight CHUG batch (~5 h per re-run) |
| Hardcode 30 fps assumption (never compensate) | Zero implementation complexity | Silently wrong on all HFR content; bias grows with frame rate | Fails lawrencecurtis's BBC 50p repro |
| Downstream normalisation pass (post-hoc score adjustment) | No changes to extractors | Requires storing raw (un-normalised) scores; breaks score-cache contracts (ADR-0298) | Architectural complexity outweighs benefit given the extractor is already parameterised |
| Document and ignore | No work | Issue #837 is a confirmed quality regression; ignoring it was explicitly rejected | Rejected per Issue #837 consensus |

## Consequences

- **Positive**: motion scores on HFR content (50p, 60p, 120p) now reflect
  actual motion energy on a 30 fps-equivalent scale; VMAF predictions on HFR
  sources are no longer systematically biased low.
- **Negative**: all previously extracted motion scores for HFR clips (e.g. the
  ~568 CHUG clips at ≥ 49 fps) are SDR-calibrated and must be treated as
  bad-data; re-extraction of the HFR subset is required (~5 h on the GPU used
  for CHUG).
- **Neutral / follow-ups**:
  - The CHUG HFR re-extraction is tracked as a follow-up in PR #851.
  - A `--hdr-aware` CLI flag exposing the same per-feature option emission to
    end users (not just the batch extractor) is deferred to a subsequent PR.
  - The CAMBI/HDR EOTF calibration gap (SDR-default CAMBI on PQ content) is
    addressed in the same code change but may be separately documented if the
    decision record needs its own ADR for traceability.

## References

- `req` — Issue #837 (lawrencecurtis, BBC 50p HFR motion under-prediction
  report): <https://github.com/VMAFx/vmafx/issues/837>
- PR #851 (fix implementation): <https://github.com/VMAFx/vmafx/pull/851>
- ADR-0434 — CHUG parquet metadata enrichment (fps / is_hdr sidecar fields
  consumed by the fps-weight detection logic)
- ADR-0214 — GPU-parity ULP gate (must cover `motion_fps_weight` clamping path)
- ADR-0298 — vmaf-tune score cache (score-cache contract context for why the
  downstream normalisation alternative was rejected)
- Secondary pattern: CAMBI HDR EOTF calibration gap — same extractor options
  anti-pattern on PQ HDR clips; addressed in PR #851; separate ADR pending if
  needed.
