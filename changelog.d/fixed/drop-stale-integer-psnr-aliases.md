Removed three unreachable second-candidate alias entries (`integer_psnr_y`,
`integer_psnr_cb`, `integer_psnr_cr`) from `_METRIC_ALIASES` in
`ai/scripts/extract_k150k_features.py`.  Neither `psnr.c` nor `integer_psnr.c`
ever emits those keys; the first candidate always matched, so the dead entries
were never reached (wiring audit 2026-05-16, Layer 3).
