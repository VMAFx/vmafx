- **`ciede_sycl` is about twice as fast at 4K on Intel Arc.** The SYCL
  ciede2000 extractor no longer upscales U and V to luma resolution on the host
  before every frame. It uploads the planes at their native size and the kernel
  reads chroma at the subsampled position, as the CUDA and HIP extractors do.
  Measured at 3840x2160 8-bit 4:2:0, `--feature ciede_sycl` drops from 17.2 to
  8.4 ms per frame on an Arc B580 and from 52.9 to 46.0 ms on a UHD 770. Scores
  are unchanged, bit for bit.
- The CLI guide corrects the `--threads` default, which is serial (`0`), not
  the host's core count.
