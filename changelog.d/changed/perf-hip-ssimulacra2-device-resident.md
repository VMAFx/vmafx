- **HIP `ssimulacra2` runs entirely on the device (ADR-1390).**
  `ssimulacra2_hip` no longer roundtrips through the host per scale; each frame
  is one upload of the raw Y/U/V planes in `submit()` and one 864-byte readback
  of per-scale totals in `collect()`. YUV-to-linear, XYB, IIR Gaussian blurs with
  a tiled shared-memory row pass (`SS2H_ROW_TILE` rows, single-wave blocks,
  two-slot ring, register prefetch), exact fp32-pair per-pixel SSIM and edge sums
  over a deterministic LDS reduction tree, and 2x2 downsample run on the device.
  On AMD gfx1036, a 3840x2160 frame takes 234.30 ms (CPU 16t takes 148.03 ms) and
  a 576x324 frame takes 5.68 ms (CPU 16t takes 2.89 ms). Scores match CPU
  reference within 1e-9 at `--precision max`: max abs diff is 1.123e-12 on
  576x324 and 5.826e-13 on 4K BBB. Rejects 4:0:0 input at init. See
  [SSIMULACRA 2](docs/metrics/ssimulacra2.md) and
  [ADR-1390](docs/adr/1390-hip-ssimulacra2-device-resident.md).
