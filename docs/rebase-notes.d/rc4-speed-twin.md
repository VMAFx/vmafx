## RC4: `speed_chroma` Rust twin keeps Netflix's double-form statements

- `core/src/rust/feature/speed/src/` is `speed.c` and `vif_tools.c` ported
  statement by statement (`BSD-2-Clause-Patent`). A change to `create_givens()`,
  `update_entropy()`, `get_speed_score()` (ADR-1477's double `sqrt()` / `log2()`
  form), `EIGENVALUE_EPS` (a double), the prescale methods, the Gaussian taps or
  `picture_copy()` changes the matching Rust function in the same PR;
  `scripts/ci/rust_twin_diff.py --feature speed_chroma` and the golden tests in
  `core/src/rust/feature/speed/src/lib.rs` fail on a one-ulp drift. An upstream
  sync that touches `speed.c` re-runs both. No score, public API or FFmpeg patch
  impact: the twin is opt-in.
