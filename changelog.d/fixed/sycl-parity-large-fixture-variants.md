- **The SYCL parity tests now also run against a 960x540 fixture.** Same
  resolution blind spot as the CUDA family (ADR-1206): every SYCL parity test
  pinned one small fixture, which held the shared SSIM/MS-SSIM auto-scale
  `max(1, round(min(w, h) / 256))` at 1 and the ADM border crop at 0, leaving
  every resolution-dependent branch unreachable. Verified on an Arc A380: 16
  variants pass, and the sweep surfaced two things worth knowing — the
  `float_ssim_sycl` twin is a documented v1 scale=1-only extractor and now
  records that contract as a skip rather than a failure, and
  `motion_add_uv` needs a separately derived tolerance at that resolution
  because it compares float CPU against fixed-point SYCL rather than two
  implementations of the same arithmetic.
