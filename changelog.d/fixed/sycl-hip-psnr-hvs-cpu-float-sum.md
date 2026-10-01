- **SYCL and HIP `psnr_hvs` return the CPU extractor's scores bit for bit**
  (`T-SYCL-PSNR-HVS-EXACT-SUM-2026-10-01`, `T-HIP-PSNR-HVS-EXACT-SUM-2026-10-01`,
  [ADR-1401](docs/adr/1401-psnr-hvs-sycl-hip-exact-twins.md)). Like the CUDA
  twin (ADR-1397), `psnr_hvs_sycl` and `psnr_hvs_hip` now store the 64 terms of
  every block in the CPU's arithmetic and the host adds them in the CPU's order.
  Before, they summed each block on the device and were up to 1.7e-2 dB from
  `--backend cpu` at 3840x2160, beyond the parity tolerance. `psnr_hvs`,
  `psnr_hvs_y`, `psnr_hvs_cb` and `psnr_hvs_cr` are identical to the CPU at
  `--precision max` from 576x324 to 3840x2160 and at 8 to 12 bits, measured on
  an Arc A380 and a gfx1036, and the parity gate compares every `psnr_hvs` cell
  with tolerance 0. The SYCL kernel has no fp64: it takes the CPU's `double`
  masking threshold from a new integer square root of the exact product
  (`sqrt_prod_rn()` in `sycl_exact_fp.h`), and it stays free of scratch memory.
  The scores of both twins therefore change in their last digits (by up to
  1.7e-2 dB at 3840x2160). Both are slower for it: a 3840x2160 frame takes
  35.9 ms instead of 22.4 ms on an Arc A380 and about 38 ms instead of 18 ms on
  a gfx1036, and the term buffer needs 65 MB per 3840x2160 frame; tuning is
  tracked as `T-SYCL-HIP-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01`. Compare a
  twin with the CPU extractor of the same `vmaf` binary: the dB value uses the
  host's `log10`, which differs by one unit in the last place between an `icx`
  and a gcc build. See
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#agreement-with-the-cpu-extractor).
