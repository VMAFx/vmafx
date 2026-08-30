# Research-0108: vmaf-roi-score saliency materialiser

- **Status**: implementation digest
- **Date**: 2026-05-14
- **Relevant ADRs**: ADR-0296, ADR-0424

## Question

What is the smallest useful way to replace the `vmaf-roi-score
--saliency-model` scaffold with a real materialiser without touching
libvmaf's numerical core?

## Findings

- The existing Option C design remains the lowest-risk surface: the
  Python tool can rewrite the distorted YUV outside salient regions and
  pass that temporary file to the unchanged `vmaf` CLI.
- `saliency_student_v1` exposes the needed ONNX contract:
  ImageNet-normalised RGB NCHW input named `input`, saliency output
  named `saliency_map`, dynamic `H` and `W`.
- 8-bit planar YUV is enough for the first shipped path because the
  existing smoke and ROI-score examples use `yuv420p`; 10/12/16-bit
  support needs separate two-byte plane fixtures and should not be
  hidden inside the first materialiser patch.

## Decision Support

The implementation should support `yuv420p`, `yuv422p`, and `yuv444p`,
infer masks from the reference frame, downsample the alpha mask for
chroma planes, and keep ONNX Runtime lazy-loaded so synthetic smoke
tests remain lightweight.

## Non-Goals

- No libvmaf C-side weighted pooling.
- No MOS-correlation claim.
- No high-bit-depth YUV materialisation in this first pass.
