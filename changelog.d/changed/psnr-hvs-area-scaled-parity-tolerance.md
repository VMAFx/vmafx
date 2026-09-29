- **Cross-backend gate: the `psnr_hvs` tolerance grows with the frame size.**
  The CPU `psnr_hvs` adds every coefficient error of a plane into one `float`,
  so its rounding error grows with the number of 8x8 blocks, and a correct GPU
  twin landed 8.4e-4 dB away at 3840x2160 against a fixed 5e-4 tolerance.
  `cross_backend_parity_gate.py` and `cross_backend_vif_diff.py` now multiply
  the `psnr_hvs` tolerance by √(N / N₅₇₆ₓ₃₂₄) above 576x324 (3.34e-3 at 4K);
  576x324 and smaller frames keep 5e-4. Both gates also stop crashing when a
  score is non-finite on both backends (JSON `null`, for example `psnr_hvs_cb`
  on identical chroma) (ADR-1361).
