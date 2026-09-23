- Fail the frame when a SpEED score is not finite, instead of publishing it as
  `speed_*_max_val`. Every backend bounded its score with a less-than
  comparison — `MIN(x, max)` on the CPU, an inline ternary on the CUDA, HIP and
  SYCL twins, a `CLIP` macro in `speed_chroma_cuda.c` — and every comparison
  against NaN is false, so all fourteen sites emitted a NaN as a finite,
  plausible 1000.0. `+Inf` was masked the same way; `-Inf` passed the
  comparison and was published unclamped. The cross-backend parity harness
  asserts `isfinite()` on what it reads, so the clamp defeated exactly the
  check meant to catch it. `speed_internal_clamp_score()` is now the single
  implementation for all fourteen sites: it checks finiteness before comparing,
  logs a warning naming the extractor, feature, frame and value, and returns
  `-EINVAL`. Finite scores clamp exactly as before. See ADR-1301.
- Guard the zero-norm case in `matrix_qr_decomposition()`
  (`core/src/feature/speed.c`), which divided the Householder vector by its own
  norm with no check. A deflated minor can leave that column exactly zero in
  float, making every lane compute `0.0f / 0.0f`; the NaN then propagated
  through `Q` and `R` into the solved system and out as a score.
  `speed_internal.c` — the copy every GPU twin uses — has always had this
  guard, so the CPU reference was the one backend that could manufacture the
  NaN. No finite result moves: for any non-zero norm the arithmetic is
  unchanged.
- Apply `speed_temporal_max_val` on the CPU. The option was declared, parsed
  and documented as "larger values will be clipped", and the CPU extractor
  never used it, while every GPU twin did — so the reference disagreed with its
  own twins for any score above the bound.
