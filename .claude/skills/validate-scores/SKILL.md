---
name: validate-scores
description: Run identical (ref, dist) through all enabled backends and report per-backend score + pairwise ULP diffs. Use to verify bit-exactness of a new SIMD path, new GPU backend, or any hot-path change.
---
<!-- markdownlint-disable MD013 -->

# /validate-scores

## Invocation

```text
/validate-scores --ref=PATH --dist=PATH --width=W --height=H --pixfmt=420p --bitdepth=8
                 [--backends=cpu,cuda,sycl,vulkan] [--precision=17]
```

## Steps

1. Each enabled backend in `--backends`, run:
   `build/tools/vmaf --reference REF --distorted DIST --width W --height H \
    --pixel_format PIXFMT --bitdepth BD --feature psnr --feature ssim --feature
    vif \
    --feature adm --feature motion --output /tmp/<backend>.json --json
    --precision=17`
2. Parse JSON; build `(frame, feature) -> score` table per backend.
3. Each backend pair, compute:
   - max absolute diff
   - max ULP distance (via `math.frexp` + integer reinterpretation)
4. Emit report: per-feature worst case across all backend pairs.
5. Exit 0 if all pair-wise ULP ≤ 2 (bit-exact or within float reduction jitter);
   exit 1 otherwise.

## Notes

- Tolerance 2 ULPs matches SIMD-vs-scalar requirement in `docs/principles.md`.
  GPU backends (CUDA / SYCL / Vulkan) NOT bit-identical to CPU (class
  invariant) — per-feature variance budget in
  [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md)
  (T6-8 GPU-parity gate) = contract for cross-device comparisons.
  Netflix golden gate = CPU-only by design.
- Higher tolerance NOT permitted without CODEOWNERS approval; backend
  divergence must be explained and justified.
- Companion skill `/cross-backend-diff` mirrors T6-8 CI gate locally.
