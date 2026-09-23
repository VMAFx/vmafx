- Fail the frame when `float_vif`, `integer_adm`, `float_ssim` or
  `float_ms_ssim` computes a non-finite score, instead of publishing it as a
  plausible number. Each bounded its score with a comparison immediately before
  appending it, and every comparison against NaN is false, so the NaN took the
  other arm: VIF published `vif_scaleN_min_val` (0.0 by default — the *worst*
  VIF value), integer ADM published `adm_min_val`, and both SSIM variants
  published `max_db` — the value their own guard reserves for perfect
  similarity. That guard reads `if (score >= 1.0) return max_db;`, which a NaN
  fails, so it fell straight through to `MIN(NaN, max_db)`. See ADR-1302.
- The Netflix golden gate is unchanged at `271 passed, 12 skipped`, and the
  clang-tidy ratchet is unmoved, so no pinned score and no debt count moves.
