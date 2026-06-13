- **chore(core):** Tighten `noexcept` and `[[nodiscard]]` annotations on
  TU-local C++ helpers in `core/src/`. Adds `[[nodiscard]]` and `noexcept`
  to `static`-linkage helpers in `feature_collector.cpp`
  (`find_feature_vector`, `feature_collector_grow_capacity`,
  `feature_collector_ensure_vector`), `fex_ctx_vector.cpp`
  (`provided_features_overlap`), `feature_name.cpp`
  (`vmaf_feature_name_from_opts_dict`, `option_is_default`), and `opt.cpp`
  (`parse_bool`, `parse_int`, `parse_double`); marks the `qsort`
  comparator lambda in `dict.cpp` as `noexcept`. No behavioural change; no
  ABI change (extern "C" entry points untouched). Surfaces ignored-return
  bugs at compile time and enables tighter codegen on the no-throw paths.
