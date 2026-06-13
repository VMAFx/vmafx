---
name: cross-backend-diff
description: Core debugging skill. Runs the same (ref, dist) through every enabled backend (cpu, cuda, sycl, vulkan), reports per-frame per-feature ULP divergence, flags anything beyond configurable tolerance. Use after any SIMD / GPU change. Mirrors the T6-8 GPU-parity CI gate (ADR-0214) locally.
---

# /cross-backend-diff

## Invocation

```text
/cross-backend-diff --ref=PATH --dist=PATH --width=W --height=H --pixfmt=420p
                    [--bitdepth=8] [--frames=48] [--tolerance-ulp=2]
                    [--backends=cpu,cuda,sycl,vulkan]
```

## Steps

1. Build every backend in `--backends` (reuses cached builds where possible).
2. Run `build/tools/vmaf` per backend with `--json --precision=17` for every feature
   of interest (default: adm, vif, motion, psnr, ssim).
3. Parse each JSON into `(backend, frame, feature) -> score` tables.
4. For every pair of backends, compute:
   - Absolute max diff
   - ULP distance (double bits XOR)
5. Report a table:

   ```text
   Feature   Pair           MaxAbsDiff   MaxULP  WorstFrame   Verdict
   adm       cpu vs cuda    3.2e-16      1       17           OK
   vif       cpu vs sycl    4.1e-15      18      23           FAIL(>2)
   ...
   ```

6. Exit 0 if all ULPs ≤ `--tolerance-ulp`, else 1.

## Notes

- ULP > tolerance is a blocker. Either fix the reduction to use double accumulation
  (see `simd-reviewer`), or open a CODEOWNERS-approved exception.
- Default test clip: the Netflix normal pair (`src01_hrc00_576x324.yuv` ↔
  `src01_hrc01_576x324.yuv`). For checkerboard variants, use the two
  `checkerboard_1920_1080_10_3_*_0.yuv` pairs.
- This skill is the local-dev mirror of the **T6-8 GPU-parity CI gate**
  ([ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md)) — the gate
  enforces the same per-feature cross-device variance budget on every PR.
  Run this skill before pushing to catch a parity break locally instead of
  burning a CI cycle.
- GPUs are NOT bit-identical to CPU as a class invariant; they're close but
  the Netflix golden gate is CPU-only by design. Don't claim CUDA/SYCL/Vulkan
  parity stricter than the per-feature variance budget in ADR-0214.
