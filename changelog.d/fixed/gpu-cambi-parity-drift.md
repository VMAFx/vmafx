- **CUDA and SYCL CAMBI scores drifted from the CPU reference on real
  content.** Both GPU twins mis-mirrored two host-side stages of
  `cambi.c`. (1) The spatial-mask kernel clamped (replicated) out-of-image
  neighbours when accumulating the 7x7 zero-derivative box sum, while the
  CPU summed-area-table zero-pads them, so every pixel within three of an
  image edge could be classified as banding when the CPU said it was not.
  (2) The vertical `filter_mode` pass overwrote output rows 0 and
  `height-1`, which the CPU's rolling three-row buffer never writes — the
  CPU leaves those two rows at their *pre-filter* values. The CUDA twin
  additionally ignored `cambi_high_res_speedup` (`hrs`): it never resolved
  the option against the encode pixel count, never applied the extra
  pre-scale-0 decimation, and never halved the adjusted window, so every
  `>= 1080p` run with the default model `vmaf_v1.0.16_3d0h` (which sets
  `hrs=1080`) scored a different pipeline than the CPU. With the fix,
  per-frame `cambi_hrs_1080_cmxv_17_vlt_0.06` is identical at `%.17g`
  across CPU, CUDA and SYCL on the 576x324 src01 pair (48 frames), the
  1080p Tennis pair (10 frames) and both 1080p checkerboard pairs.
