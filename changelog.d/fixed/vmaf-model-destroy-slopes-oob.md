- `vmaf_model_destroy` no longer triggers a heap-buffer-overflow read
  when fed a JSON model whose per-feature arrays (`slopes`,
  `intercepts`, `feature_opts_dicts`) disagree in length with
  `feature_names`. The parser now rejects such models with `-EINVAL`
  at load time, and the destructor walks `min(feature_cap, n_features)`
  defensively. Surfaced by `fuzz_json_model` (PR #371); tracked as
  T-JSON-MODEL-SLOPES-FEATURE-CAP-OOB-2026-05-30 in `docs/state.md`;
  decided in ADR-0887.
