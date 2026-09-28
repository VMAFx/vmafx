- `cambi_sycl` now runs every stage on the GPU — preprocessing, the spatial
  mask, the per-scale decimation and mode filter, the sliding-histogram
  c-values and the top-K pooling — and reads back one 88-byte block per
  frame instead of downloading the image and mask and computing the c-values
  on the host at every scale (ADR-1357). It reads the distorted plane from
  the shared SYCL frame upload and rides the combined command graph with the
  other SYCL extractors. At 3840x2160 (Big Buck Bunny, `--feature cambi_sycl`,
  ms/frame from `t(22) - t(2)`) the Arc B580 goes from 140 to 9.3 and the
  UHD 770 from 944 to 42; the default model on the UHD 770 goes from 976 to
  103 at 4K and from 138 to 20 at 576x324. Scores are bit-identical to `--backend cpu` whenever the CPU's
  own top-K double sum is exact (every 576x324 and 1080p fixture tested, 47
  of 50 Big Buck Bunny 4K frames); on the other frames the device returns the
  exactly rounded pooled mean and the CPU differs by its summation rounding
  (at most 2.2e-15 on Big Buck Bunny). `cambi.c` exports its reciprocal table
  as `vmaf_cambi_reciprocal_lut()` for GPU twins. The CUDA, HIP and Metal
  twins keep the host residual (RC3 rows in `docs/state.md`).
