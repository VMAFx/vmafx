- **`ssimulacra2_sycl` runs entirely on the device, and `float_ms_ssim_sycl`
  waits once per frame (ADR-1363).** The SYCL ssimulacra2 twin no longer
  converts colour, computes XYB, downsamples or combines the SSIM and
  edge-difference maps on the host between device passes, and no longer copies
  five full-size buffers back per scale: each frame is one upload of the raw
  planes and one 864-byte readback. On an Arc B580 a 3840x2160 frame takes
  33 ms instead of 963 (the CPU extractor on 16 threads takes 167); on a UHD
  770, 445 instead of 1025. Its score is within about 1e-11 of `--backend cpu`
  (6.7e-12 at worst on the tested content, identical on every device) where it
  used to match bit for bit: the device has no fp64 and sums the per-pixel
  terms in a fixed tree of exact fp32 pairs. `float_ms_ssim_sycl` now enqueues
  every scale in `submit()` and waits once in `collect()` instead of once per
  scale; its output is unchanged. 4:0:0 input is rejected by `ssimulacra2_sycl`
  at init. `scripts/dev/speed_gpu_parity.py` takes `--feature` and
  `--max-abs-diff` to check and time any GPU twin. See
  [SSIMULACRA 2](docs/metrics/ssimulacra2.md).
