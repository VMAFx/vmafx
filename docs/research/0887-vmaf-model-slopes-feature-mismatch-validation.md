<!-- markdownlint-disable MD013 -->
# Research-0887: `vmaf_model_destroy` heap-buffer-overflow from per-feature array length mismatch

- **Date**: 2026-05-30
- **Owner**: lusoris
- **Status**: Closed (ADR-0887 accepted, PR fix/vmaf-model-destroy-slopes-oob)
- **Linked**: [ADR-0887](../adr/0887-vmaf-model-slopes-feature-mismatch-validation.md), PR #371

## Question

Why does `vmaf_model_destroy` (`core/src/model.c:208`) trip a
heap-buffer-overflow read under ASan on the `fuzz_json_model`
reproducer
`core/test/fuzz/json_model_known_crashes/slopes_oob_destroy.bin`
(committed in PR #371), and what is the minimal-blast-radius fix?

## Evidence

### Reproduction

Build with `-Db_sanitize=address` against `origin/master`, drive the
reproducer through the public API:

```c
VmafModel *m = NULL;
VmafModelConfig cfg = {0};
int rc = vmaf_model_load_from_path(&m, &cfg, "slopes_oob_destroy.json");
if (rc == 0) vmaf_model_destroy(m);
```

ASan output (pre-fix):

```text
==…==ERROR: AddressSanitizer: heap-buffer-overflow on address …
READ of size 8 at … thread T0
    #0 vmaf_model_destroy core/src/model.c:210
    #1 vmaf_read_json_model … (fail path)
    #2 vmaf_read_json_model_from_path …
    #3 vmaf_model_load_from_path …
```

(See `==2038680==ABORTING` in the verification trace; full output
captured in the PR description.)

### Root cause walk

The fuzzer reproducer contains repeated `feature_names` keys (mangled
duplicates from JSON-token splice mutations). Each `parse_feature_names`
call:

1. Resets local `i` to 0 (loop variable).
2. Calls `append_feature_name(model, name, i++)` once per name —
   which calls `ensure_feature_capacity(i + 1u)`. Capacity grows only
   when `needed > cap`; the initial `MODEL_FEATURE_INITIAL_CAP = 8`
   covers small inputs.
3. Increments `model->n_features` unconditionally per name.

For N repeated `feature_names` keys with M names each, the final
`n_features = N * M`. Capacity stays at the initial 8 (when
`M ≤ 8`). When `N * M > 8`, `vmaf_model_destroy`'s
`feature_count = max(feature_cap, n_features)` formula picks
`n_features`, and the loop walks past the malloc'd buffer.

A parallel — but distinct — problem exists in `parse_slopes`,
`parse_intercepts`, and `parse_feature_opts_dicts`: they grow
`feature_cap` via `ensure_feature_capacity(i+1u)` per per-feature
value, but never update `n_features`. A well-formed-JSON model with
`slopes.length > feature_names.length` would similarly leave
`feature_cap` and `n_features` out of sync — silently broken (no OOB
in that direction since `max` keeps the walk within `feature_cap`)
but still a contract violation; downstream consumers walking
`[0, n_features)` would see fewer features than the parser actually
populated.

### Fix shape (per user direction)

1. Parse-time validation: every per-feature walker syncs
   `n_features = max(n_features, i)` via a new `sync_n_features`
   helper. `parse_feature_names` switches from unconditional
   `n_features++` to the same max-merge.
2. End-of-`parse_model_dict` cross-key validator
   (`validate_feature_arrays`) walks `[0, n_features)` and rejects
   with `-EINVAL` if any slot lacks a `name` (which only
   `parse_feature_names` populates) — catches slopes/intercepts/opts
   longer than feature_names.
3. `vmaf_model_destroy` walks `min(feature_cap, n_features)` —
   belt-and-suspenders defence so a future regression that drifts
   `n_features` past `feature_cap` cannot become an OOB read in the
   destructor.

### Verification

Pre-fix (origin/master `387839eacf`): ASan heap-buffer-overflow,
abort.
Post-fix (this branch): same driver returns `-EINVAL`, no ASan
report; 45/45 `test_model` unit tests green (including the 3 new
regression tests); 49/49 fast suite green.

## Decision

Apply the three-part fix in [ADR-0887](../adr/0887-vmaf-model-slopes-feature-mismatch-validation.md).

## Follow-ups

- When PR #371 lands, its reproducer bin becomes the permanent fuzz
  regression input for this defect (it lives under
  `core/test/fuzz/json_model_known_crashes/`).
- The `validate_feature_arrays` hook is the natural home for
  future per-feature schema checks (e.g. enforcing that
  `feature_opts_dicts` length matches `feature_names`, currently
  allowed to be shorter for back-compat with shipped models that
  omit dicts).
