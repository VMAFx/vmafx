## Fixed

- **UBSan enum-invalid-value in `vmaf_log` and `vmaf_option_set`** (ADR-1080): cast
  enum parameters to `int` before comparisons in `vmaf_log` (`log.cpp`) and before
  the `switch` in `vmaf_option_set` (`opt.cpp`). Eliminates UBSan `enum-invalid-value`
  runtime errors when callers pass out-of-range values (e.g. `VMAF_LOG_LEVEL_NONE-1`
  or `(VmafOptionType)9999`). Fixes `test_log` and `test_opt` under the
  ASan+UBSan PR gate.
