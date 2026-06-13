<!-- markdownlint-disable MD013 MD060 -->
# ADR-0430: Saliency RGB ingest and SSIMULACRA2 public docs

- **Status**: Accepted
- **Date**: 2026-05-14
- **Deciders**: Lusoris, Codex
- **Tags**: vmaf-tune, saliency, docs, metrics, fork-local

## Context

The public-doc gap scan found two user-facing stale surfaces: the
SSIMULACRA2 metric page still called itself a stub even though scalar, SIMD,
and GPU implementations are in tree, and the `vmaf-tune` saliency section
still documented luma-replicated RGB as a deferred limitation.

The saliency student was trained for ImageNet-normalised RGB. Feeding luma
replicated into all channels is a defensible smoke path, but it discards
available chroma from the yuv420p source. The existing saliency pipeline
already accepts yuv420p input and has a NumPy preprocessing step, so the
implementation cost of nearest-neighbour chroma upsample plus BT.709
limited-range conversion is small.

## Decision

`vmaf-tune` saliency inference will read full yuv420p frames, upsample U/V to
luma resolution, convert BT.709 limited-range YUV to RGB, and then apply the
existing ImageNet normalisation before calling `saliency_student_v1`. The
SSIMULACRA2 public metric page will become an operator reference rather than a
stub index page.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep luma-replicated RGB | Fastest and already tested | Leaves a documented deferred limitation in a user-facing saliency path; ignores colour cues the model can consume | Rejected |
| Convert yuv420p to RGB in the saliency preprocessor | Closes the deferred limitation; preserves the existing model contract; easy to test without ffmpeg | Slightly more CPU per sampled frame; chroma upsample remains nearest-neighbour | Chosen |
| Shell out to ffmpeg for RGB frames | Delegates colour conversion to a mature implementation | Adds a subprocess dependency to the hot saliency path and complicates tests | Rejected |
| Leave `docs/metrics/ssimulacra2.md` as a stub index | No code/docs churn | Contradicts the shipped implementation status and the doc sweep heuristic | Rejected |

## Consequences

- **Positive**: Saliency ROI can use colour information from source clips, and
  users now get a direct SSIMULACRA2 reference page with invocation, output,
  input formats, backends, and limitations.
- **Negative**: Saliency preprocessing does a small amount of extra NumPy work
  per sampled frame.
- **Neutral / follow-ups**: The saliency path still documents aggregate
  per-clip masks and nearest-neighbour chroma upsampling. Per-frame ROI remains
  separate future work.

## References

- [ADR-0293](0293-vmaf-tune-saliency-aware.md)
- [ADR-0130](0130-ssimulacra2-scalar-implementation.md)
- [ADR-0164](0164-ssimulacra2-snapshot-gate.md)
- `req`: "when i look at the human facing docs we only need to search for (stub) or stub and for \"limitations\" or \"deferred\" to find the next tasks lol (perhaps we can combine some of them to make it a few less pr's lol)"
