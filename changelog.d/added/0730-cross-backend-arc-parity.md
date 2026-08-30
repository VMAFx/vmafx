- **Intel Arc A380 cross-backend numerical parity research
  ([Research-0730](../../docs/research/0730-cross-backend-arc-parity-20260527.md)).**
  First systematic measurement of SYCL (Level Zero) and Vulkan (Mesa ANV)
  numerical parity on Intel Arc A380 (DG2) against the CPU scalar reference.
  Tested 16 features × 2 backends (cpu↔sycl, cpu↔vulkan) at 576×324 48 frames.
  13/16 features pass places=4 on both backends.
  Failures: `float_ssim` (fp32 accumulation on fp64-less hardware),
  `float_ansnr` (within the ADR-0194 places=3 relaxed contract),
  `ssimulacra2` (XYB/IIR transcendental divergence ~10× over places=2),
  `cambi` (Strategy II GPU hybrid returns all-zero scores — correctness bug).
  Recommendation: Arc A380 is not ready for required CI status until the
  `float_ssim`/`ssimulacra2` calibration entries are added (ADR-0234) and the
  cambi zero-score bug is fixed. Viable as an informational lane for the 13
  passing features today.
