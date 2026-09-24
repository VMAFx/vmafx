- **Resolve live GitHub CodeQL cpp/equality-on-floats alerts (ADR-1308)**:
  All six live alerts on current origin/master are resolved at their semantic root
  cause without scanner suppression comments, query evasion, public-ABI expansion,
  score/tolerance weakening, or Netflix golden changes. Replaced raw float equality
  checks with exact contract-preserving comparisons: IEEE-exact option comparison
  (`option_double_equals`, where NaN is never equal, signed zeros match, same
  infinities match, and finite values compare via 64-bit bit identity) in
  `core/src/feature/feature_name.cpp`, sentinel score check (`float_values_equal`,
  where NaN is never equal, signed zeros match, same infinities match) in
  `core/src/predict.c`, discrete label equality (`svm_labels_equal`, 64-bit bit
  identity with signed zero equivalence and same-infinity behavior, rejecting NaN)
  in `core/test/test_svm_api.c`, constant-difference span check (`span != 0.0`)
  in `brisque_range_scale` in `core/src/feature/brisque_math.h` along with HISS-04
  decomposition of `brisque_fit_aggd` into `brisque_aggd_accumulate`,
  compiler-float-model-preserving difference check in
  `core/src/mcp/3rdparty/cJSON/cJSON.c`, and AVX2 vs scalar parity assertion
  (`float_bits_equal` in `check_c_values_avx2_parity`) in `core/test/test_cambi.c`.
  Validated clean on fresh CodeQL 2.27.0 whole-project database analysis and
  complete test gates.
