---
paths:
  - core/src/feature/integer_adm.c
  - core/src/feature/integer_adm.h
  - core/src/feature/integer_adm_kernels.h
  - core/src/feature/adm_csf_fixed_point.h
  - core/src/feature/adm_cm_accumulator.h
  - core/src/feature/adm_angle_flag.h
  - core/src/feature/adm_score.h
  - core/src/feature/barten_csf_tools.h
  - core/src/rust/feature/adm/**
invariant: Integer ADM arithmetic change in C = same change in core/src/rust/feature/adm; rust_twin_diff.py adm shows 0 diffs.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer ADM Rust twin (`adm_rust`, ADR-1713)

- **Two implementations of one extractor.** `core/src/rust/feature/adm/` ports
  scalar path of `integer_adm.c` and headers above statement by statement:
  same integer widths and narrowing points, same float evaluation order, same
  libm calls (`vmafx_fex::libm`). `adm_rust` must equal `adm` bit for bit on
  every option. Change to arithmetic, option table, emitted names or their
  order on C side changes crate in same PR; upstream sync touching these files
  re-runs harness.
- **Map C -> Rust.** DWT and index tables: `dwt.rs`; decouple and angle flag:
  `decouple.rs`; CSF stage: `csf.rs`; denominators: `den.rs`; contrast
  masking: `cm.rs`; CSF weights (Watson, Barten, blended tables, fixed-point
  normalisation): `csf_weights.rs`; literal tables: `tables.rs`; borders and
  shift budgets: `region.rs`; finalisation and emission: `score.rs`; driver:
  `lib.rs`.
- **Quirks kept on both sides.** `add_bef_shift_flt = (int32_t)(1u << 31)`
  (Netflix#955, ADR-0155), scale-0 diagonal rounding term `65535`, int32
  centre tap (ADR-1402), int64 clamped scale-0 excess, unclipped integer AIM
  (ADR-1417), `uint32_t` exponent of scale 1..3 denominator, `int64_t / float`
  division of scale 1..3 numerator.
- **Float tables.** C tables are double literals narrowed to `float`; crate
  writes each as `f(<double literal>)`. `tables.rs` holds every entry against
  C bit pattern (`tables_c.rs`, dumped from both headers).
- **Constants C compiler folds** (`cos(M_PI/180)^2`, Barten luminance anchors,
  `barten_rod_cone_sens(100)`): crate computes them with glibc at init; equal
  to GCC's folded values on glibc (unit tests pin bits). Other libm that
  differs: write C value as literal, never guess.
- **Evidence.** `scripts/ci/rust_twin_diff.py --feature adm` over `netflix`,
  `checker1`, `checker10`, `sparks10`, `bbb4k` for default and four
  `vmaf_v1.0.16*` option sets; `cargo test -p vmafx-fex-adm`.
