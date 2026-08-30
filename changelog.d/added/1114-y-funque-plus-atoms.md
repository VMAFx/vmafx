- **`y_funque_plus` feature extractor** (`core/src/feature/y_funque_plus.c`):
  a CPU full-reference wavelet-domain metric shipping the three Y-FUNQUE+
  **atom features** — `y_funque_plus_ms_ssim` (MS-SSIM with covariance
  pooling), `y_funque_plus_dlm` (DLM detail-loss measure), and
  `y_funque_plus_mad` (MAD-Ref temporal atom). Per frame, on the luma plane
  only: a 2x OpenCV `INTER_CUBIC` pre-downscale (Keys cubic `a = -0.75`,
  `BORDER_REPLICATE`), a crop to a multiple of `2^levels`, a 2-level Haar DWT
  (pywt `'periodization'` convention), Nadenau Y-channel CSF weighting of the
  detail subbands only, then the three atoms. All arithmetic is double
  precision (`-ffp-contract=off`, own static library, mirroring
  `ssimulacra2`). Reachable via `vmaf --feature y_funque_plus`, the libvmaf C
  API, and the feature registry. The extractor is temporal (the MAD-Ref atom
  caches the previous frame's final approx subband; it reports 0 on frame 0).
  ADR-1114.
- **Scope (atoms-only):** the fused Y-FUNQUE+ MOS score is **deferred** —
  upstream `funque_plus` ships no frozen regressor (it trains a per-dataset
  `ScaledSVR` at runtime), so a fused number would be fork-originated and
  needs a licensed subjective dataset plus a model card. Consumers combine the
  three atoms themselves until a fork-trained SVR lands (tracked in
  `docs/state.md`).
- **License:** the reference `funque_plus` implementation is MIT
  (Copyright (c) 2023 Abhinau Kumar), compatible with the fork's
  BSD-2-Clause-Patent; the C extractor is a clean-room reimplementation from
  the published papers (arXiv:2304.03412, arXiv:2202.11241) cross-checked
  against the MIT reference, with no Python source copied verbatim.
- **`core/test/test_y_funque_plus.c`**: six tests asserting the analytic
  identical-input oracles (ms_ssim = 0, dlm = 1, mad = 0 at 8x8, odd 65x33,
  and the 100x100 crop path), a 64x64 non-trivial Python-reference oracle and
  a 2-frame MAD-Ref temporal oracle (both at places=4), and a too-small-frame
  `init()` rejection.
- **`docs/metrics/y-funque-plus.md`**: user-facing documentation (what the
  atoms measure, the atoms-only scope and deferred fused score, SDR/HDR
  limitations, options, CLI usage, and references).
