- **Resolve live GitHub CodeQL cpp/equality-on-floats alerts (ADR-1308)**:
  All six live alerts on current origin/master are resolved at their semantic root
  cause without scanner suppression comments, query evasion, public-ABI expansion,
  score/tolerance weakening, or Netflix golden changes. Replaced raw float equality
  checks with exact contract-preserving comparisons: NaN-aware and bit-identity
  option comparison (`option_double_equals`) in `core/src/feature/feature_name.cpp`,
  sentinel score check (`float_values_equal`) in `core/src/predict.c`, discrete label
  equality (`svm_labels_equal`) in `core/test/test_svm_api.c`, constant-difference span
  check (`span != 0.0`) in `core/src/feature/brisque_math.h` along with HISS-04
  decomposition of `brisque_fit_aggd` into `brisque_aggd_accumulate`, DAZ-preserving
  difference check in `core/src/mcp/3rdparty/cJSON/cJSON.c`, and bit-pattern
  assertion (`float_bits_equal`) in `core/test/test_cambi.c`. Validated clean on a
  fresh CodeQL whole-project database analysis and complete test gates.
