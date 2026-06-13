<!-- markdownlint-disable MD060 -->
# ADR-0425: vmaf-roi-score saliency materialiser

- **Status**: Accepted
- **Date**: 2026-05-14
- **Deciders**: Lusoris
- **Tags**: tooling, ai, saliency, vmaf, fork-local

## Context

ADR-0296 shipped `vmaf-roi-score` as an Option C scaffold: run `vmaf`
twice, blend the full-frame and saliency-masked scores, and defer the
actual YUV mask materialiser until `saliency_student_v1` was available.
That dependency is now present in `model/tiny/saliency_student_v1.onnx`,
and leaving `--saliency-model` as an exit-64 path makes the user-facing
tool unusable for its main purpose.

The materialiser must stay out of libvmaf C-side pooling. Option A
remains a separate future decision because it affects numerical
correctness and cross-backend parity. This follow-up only fills the
Python tool-level substitution path described by ADR-0296.

## Decision

We will wire `vmaf-roi-score --saliency-model` to materialise a
temporary 8-bit planar YUV file by running ONNX saliency inference on
the reference frame, replacing low-saliency distorted pixels with
reference pixels, and then scoring that temporary file with the
existing `vmaf` CLI. The first implementation supports `yuv420p`,
`yuv422p`, and `yuv444p`; higher-bit-depth formats remain rejected with
a clear error.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Implement 8-bit YUV materialisation in the Python tool | Completes the documented Option C path; no libvmaf numerical drift; easy to test with injected masks | Limited to 8-bit planar YUV; costs a second `vmaf` run | Chosen as the smallest complete user-facing implementation |
| Extend directly to 10/12/16-bit YUV | Covers HDR and high-bit-depth workflows immediately | Requires separate plane-width handling and more fixtures; larger failure surface | Deferred so the first usable path stays small and auditable |
| Implement Option A in libvmaf pooling | More mathematically direct and one-pass | Touches numerical core, model semantics, and cross-backend parity | Out of scope for this follow-up; remains future ADR work |
| Keep `--saliency-model` scaffolded | No new runtime dependency pressure | User-visible CLI remains mostly unusable | Rejected because `saliency_student_v1` now unblocks the path |

## Consequences

- **Positive**: `vmaf-roi-score --saliency-model` now produces a real
  masked YUV and score instead of returning exit 64.
- **Positive**: the implementation keeps ONNX Runtime lazy-loaded, so
  synthetic-mask smoke tests do not require runtime extras.
- **Negative**: 10/12/16-bit YUV users still need the ordinary
  full-frame path or a future materialiser extension.
- **Neutral / follow-ups**: Option A, true per-pixel weighted pooling
  inside libvmaf, remains deferred and must not be folded into this
  Python tool follow-up.

## References

- [ADR-0296](0296-vmaf-roi-saliency-weighted.md)
- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md)
- [Research-0069](../research/0069-vmaf-roi-saliency-weighted.md)
- Source: req ("just do one")
