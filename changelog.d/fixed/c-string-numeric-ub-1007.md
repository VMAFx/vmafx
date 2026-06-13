- **fix(core)**: eliminate five C UB sources in `feature_name.c`, `model.c`,
  `integer_adm.c`, and `cambi.c` — NULL `strcmp` guard, `size_t` underflow guard
  on model-name subtraction, signed-integer left-shift literals changed to `1u`,
  `uint32_t` underflow guard on `pow(2, shift-1)`, and `snprintf` truncation check
  for `heatmaps_path` (ADR-1007).
