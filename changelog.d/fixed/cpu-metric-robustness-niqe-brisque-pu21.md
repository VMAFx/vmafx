### Fixed

- **CPU no-reference metric robustness (NIQE / BRISQUE / PU21 docs)**: Three
  RC-audit hardening fixes on the CPU metric path.
  - **NIQE AGGD fit NaN guard.** `niqe_extract_aggd()` only guarded
    `mean_sq == 0`; on a reachable all-negative MSCN patch (`rms == 0`,
    `mean_sq != 0`) the `gamma_hat = lms/0 = inf` propagated to
    `rhat_norm = inf/inf = NaN`, which trips the `isfinite` assert under a
    sanitizer/debug build. The fix mirrors BRISQUE's two-sided guard and the
    Python harness exactly: when `rms == 0` it selects gamma index 0
    (`alpha = 0.2`), yielding `bl = aggdratio*lms`, `br = 0`,
    `N = (br-bl)*(g2/g1)*aggdratio` — the same values numpy produces
    (`np.argmin` of an all-NaN array returns index 0). RELEASE output is
    unchanged; this only alters the degenerate `rms == 0` path.
  - **BRISQUE assert-as-guard.** `assert(isfinite(score))` after `svm_predict`
    was a no-op in release and an abort in debug. Replaced with a runtime
    finite guard that logs a warning and returns `-EINVAL` (mirrors the
    `y_funque_plus.c` non-finite-atom guard).
  - **PU21 range documentation.** Documented in the `transfer` option help and
    `docs/metrics/pu21.md` that the PQ decode assumes FULL-RANGE code values
    (normalised by `2^bpc - 1`); limited-range HDR10 would be mis-scaled. No
    `range` option is added — PU21 does no YUV→RGB matrix decode.
