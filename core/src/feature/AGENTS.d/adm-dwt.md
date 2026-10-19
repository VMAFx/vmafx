---
paths:
  - core/src/feature/integer_adm.c
  - core/src/feature/integer_adm.h
invariant: Integer ADM DWT mirror table for tiny extents and 16-bit vertical int64 sums.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer ADM DWT Mirroring and 16-Bit Vertical Sums

- **`integer_adm.c` DWT mirror table for tiny extents** (fork-only fix,
  [Research-2063](../../../../docs/research/2063-upstream-sync-2026-09-adm-vif-simd.md)):
  `dwt2_src_indices_1d()` starts its mirrored tail at
  `(n_half > 2u) ? n_half - 2u : 1u` and bounds first loop with
  `i + 2 < n_half`. Upstream's `n_half - 2` restarts tail at 0 when
  `n_half == 2` (scale 3 for any frame dimension from 17 to 32), replaces
  `{1, 0, 1, 2}` mirror with `{-1, 0, 1, 2}` and reads index -1 before
  band and before `tmp_ref` allocation. `init_buffers()` also zeroes
  `data_buf` (upstream `1786bd961`), but only as defence in depth:
  zeroing does not make upstream's bound safe. Guarded by
  `test_integer_adm_tiny_frames`, which ASan lane aborts on old bound.

## Integer ADM's 16-bit vertical DWT sums in int64

- `adm_dwt2_vpass16_tap4()` (`integer_adm.h`) = only 16-bit vertical DWT
  response. Scalar `adm_dwt2_vpass_16()`, `adm_dwt2_16_avx2()`,
  `adm_dwt2_16_avx512()` call it.
- Low-pass taps 1-3 sum 50582 -> int32 partial sum overflows at 16 bpc once
  3 samples >= 42456. Upstream form = int32 = UB.
- Normalised result fits int32 -> int64 form bit-exact with old wrap. Never
  "optimise" back to int32; outputs match, UB returns.
- Guard: `test_integer_adm_dwt16_range` (sanitizer lane halts on UB).
- 8-bit pass stays int32: 255 * 50582 fits.
- T-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18.
