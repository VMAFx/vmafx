- **Cross-backend parity matrix** (`docs/research/0550-cross-backend-parity-matrix-2026-05-18.md`):
  systematic audit of all 18 CPU feature extractors across SYCL (Intel Arc A380),
  CUDA (RTX 4090), and Vulkan on the Netflix 576x324 golden fixture.
  All 18 extractors (`float_psnr`, `float_ansnr`, `float_adm`, `float_vif`,
  `float_motion`, `float_moment`, `float_ssim`, `float_ms_ssim`, `psnr`, `psnr_hvs`,
  `adm`, `motion`, `motion_v2`, `vif`, `cambi`, `ssimulacra2`, `ciede`, `ssim`)
  are bit-exact vs CPU at IEEE-754 double precision (`--precision max`). No P0 or P1
  divergence found. Eliminates the previously documented 3.1e-5 ADM-scale1 delta.
  Registration coherence gaps for `speed_chroma`, `speed_temporal`, and integer `ssim`
  are documented. HIP parity deferred pending a host with a discrete AMD GPU. ADR-0550.
