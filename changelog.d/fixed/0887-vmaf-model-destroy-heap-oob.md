- `vmaf_model_destroy` no longer triggers a heap-buffer-overflow read when
  fed a JSON model whose per-feature arrays (`slopes`, `intercepts`,
  `feature_opts_dicts`) disagree in length with `feature_names`, or when
  the JSON contains repeated `feature_names` keys. The parser now rejects
  such models with `-EINVAL` at load time via `validate_feature_arrays`,
  and the destructor walks `min(feature_cap, n_features)` defensively.
  Surfaced by `fuzz_json_model` nightly run (ADR-0887).
