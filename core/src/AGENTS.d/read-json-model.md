---
paths:
  - core/src/read_json_model.c
  - core/src/read_json_model.h
invariant: Model feature arrays sync capacity before access and free previous names before strdup.
area: models-and-rust
---
<!-- markdownlint-disable MD013 -->
# JSON model parser feature array capacity and name lifecycle

## 10. `read_json_model.c` — `n_features` / `feature_cap` invariant (ADR-0887)

Every per-feature walker (`parse_slopes`, `parse_intercepts`,
`parse_feature_opts_dicts`, `parse_feature_names`) must call
`sync_n_features(model, i)` so `model->n_features` = max-merge of every
walker's per-iteration high-water mark. Contract checked by
`validate_feature_arrays` at end of `parse_model_dict`:

- For every slot `[0, n_features)`, `feature[i].name` must be non-NULL
  (only `parse_feature_names` populates names).
- `feature_cap >= n_features` guaranteed by `ensure_feature_capacity`
  inside every walker.

Never:

- Reintroduce unconditional `model->n_features++` in `parse_feature_names`
  (prior shape double-counted on fuzzer-mangled JSON with repeated
  `feature_names` keys; ADR-0887 reproducer).
- Add new per-feature walker without calling `sync_n_features`. Even if
  walker only touches existing per-slot field (e.g. future
  `feature[i].chroma_correction`), `feature_cap` and `n_features` drift
  without sync.
- Loosen `validate_feature_arrays` rejection back to warning.
  Surfacing contract violation as `-EINVAL` at parse time =
  ADR-0887 invariant preventing OOB-read shape from re-emerging in
  `vmaf_model_destroy`.
- Drop `free(model->feature[index].name)` that precedes `strdup`
  in `append_feature_name` (both `read_json_model.c` and C++23 twin
  `read_json_model.cpp`). Duplicate `feature_names` key re-runs
  `parse_feature_names` from index 0 and overwrites `feature[index].name`;
  without free, prior strdup'd name is orphaned. `vmaf_model_destroy`
  walks only current slot occupants, so orphan is unreachable and
  leaks on both validation-error and success paths (nightly
  `fuzz_json_model` LeakSanitizer lane, `Direct leak of N byte(s)`). Keep
  two parser variants in lockstep — leak is identical in both.

When porting upstream Netflix/vmaf commit modifying
`core/src/read_json_model.c` or `core/src/model.c::vmaf_model_destroy`,
keep both `sync_n_features` calls and `min(feature_cap, n_features)`
bound in destroy, plus `free`-before-`strdup` guard in
`append_feature_name`; re-apply fork's hunks on top of any upstream
changes.
