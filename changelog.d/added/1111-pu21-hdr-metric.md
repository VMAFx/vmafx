- **PU21 HDR perceptual metric** (`core/src/feature/pu21.c`, `pu21_math.h`,
  `pu21_ssim.{c,h}`): new CPU extractor `pu21` providing two features,
  `pu21_psnr` and `pu21_ssim`. It maps the luma plane through the PU21
  (Perceptually Uniform encoding, 2021) transfer function so the SDR PSNR/SSIM
  metrics become perceptually meaningful on HDR content (Mantiuk & Azimi,
  QoMEX/PCS 2021). RC ships PQ (SMPTE ST.2084) input only: the PQ-coded luma is
  decoded to absolute cd/m² via the ST.2084 EOTF × 10000, clamped to
  [0.005, 10000], then PU21-encoded (default `banding_glare` variant; all four
  selectable via `variant`). PU-PSNR uses peak = 256 with no SDR dB cap; PU-SSIM
  uses a self-contained L = 256 Gaussian SSIM that does **not** modify the
  golden `float_ssim`/`iqa_ssim` (L = 255). Options: `variant`
  (default `banding_glare`), `transfer` (default `pq`; non-pq → `-EINVAL`,
  HLG/SDR deferred). All per-pixel math is double precision. ADR-1111.
- **`core/test/test_pu21.c`**: correctness test against the verified design
  dossier oracle — the `banding_glare` encoder points (places=6), the
  PU-PSNR(100,99) = 51.873338803 dB end-to-end oracle (places=4), the
  identical-plane PU-PSNR finiteness guard, the PQ EOTF peak (10000 nits), and
  PU-SSIM = 1.0 for identical planes. Passes under `MALLOC_PERTURB_`.
- **`docs/metrics/pu21.md`**: user-facing documentation (what it measures,
  PQ-only RC scope, both feature keys, options, references).
