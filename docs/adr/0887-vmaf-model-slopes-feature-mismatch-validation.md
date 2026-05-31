<!-- markdownlint-disable MD060 -->
# ADR-0887: Reject JSON models whose per-feature arrays disagree on length

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: security, parser, model, fuzz, hardening

## Context

`fuzz_json_model` (PR #371, ADR-0882) surfaced a heap-buffer-overflow read
in `vmaf_model_destroy` (`core/src/model.c:208`) on its first run against
the libvmaf SVM-model JSON parser. The defect chain:

1. `core/src/read_json_model.c::parse_slopes` /
   `parse_intercepts` / `parse_feature_opts_dicts` call
   `ensure_feature_capacity(i + 1u)` once per per-feature value, growing
   `model->feature_cap` without updating `model->n_features`.
2. `core/src/read_json_model.c::parse_feature_names` unconditionally
   incremented `model->n_features` once per name parsed — including on
   repeated `feature_names` keys in fuzzer-mangled JSON, where each
   re-parse reset its local `i` to 0 but still bumped `n_features` past
   `feature_cap`.
3. `vmaf_model_destroy` walked `max(feature_cap, n_features)` slots,
   reading past the malloc'd `feature[]` buffer on any state where
   `n_features > feature_cap`.

The fuzzer reproducer
`core/test/fuzz/json_model_known_crashes/slopes_oob_destroy.bin` (PR #371)
triggered the OOB; tracked as
`T-JSON-MODEL-SLOPES-FEATURE-CAP-OOB-2026-05-30` in
[`docs/state.md`](../state.md).

Forces: a JSON model with mismatched per-feature array lengths is
malformed by VMAF's contract — every `feature_names[i]` must have a
corresponding `slopes[i+1]`, `intercepts[i+1]`, and (when present)
`feature_opts_dicts[i]`. Silently accepting a partial / over-long set
is what makes downstream destructors and predict-time walkers vulnerable.
CERT C MEM35-C ("Allocate sufficient memory for an object") and
NASA Power of 10 rule 7 ("Check the return value of every non-void
function … and check the value of every parameter") both argue for
input-validation at the parse boundary.

## Decision

We will validate per-feature array lengths at parse time and reject
mismatched models with `-EINVAL`. Concretely:

1. `parse_slopes` / `parse_intercepts` / `parse_feature_opts_dicts` /
   `parse_feature_names` all use a new `sync_n_features(model, count)`
   helper that max-merges their per-iteration count into
   `model->n_features` (instead of the old unconditional `n_features++`
   in `parse_feature_names` and the no-update behaviour in the other
   three walkers).
2. After consuming all `model_dict` keys, a new `validate_feature_arrays`
   pass walks `[0, n_features)` and rejects with `-EINVAL` if any slot
   below the high-water mark lacks a `name` (which only
   `parse_feature_names` populates).
3. `vmaf_model_destroy` bounds its walk by `min(feature_cap, n_features)`
   instead of `max(...)`. Belt-and-suspenders: with the parse-time
   validation in place the two values match for well-formed models, but
   `min` prevents any future regression that drifts `n_features` past
   `feature_cap` from re-introducing the OOB-read shape.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(chosen) Parse-time validation + `min` bound in destroy** | Catches malformed input early, defends destroy as a second layer, keeps the rejection visible in error logs. | Two-line change in each walker plus a final validator; +3 unit tests. | Matches the user direction ("surface bad data at parse time, not on destroy. Add a defensive bound in destroy as belt-and-suspenders"). |
| Destroy-only `min(feature_cap, n_features)` bound | Smallest patch (1 line). | Silently accepts malformed models — downstream `vmaf_predict_score_at_index` would still walk a partially-populated `feature[]` and either crash later or return garbage scores. Hides the bug from operators. | Defence-in-depth without input validation leaves the contract violation unreported. |
| Reset `model` to zero state on every key reparse | Would normalise the multi-call `feature_names` shape. | Loses any earlier valid state (operator who appends a `feature_opts_dicts` after `feature_names` would see their dicts erased). Surprising semantics. | Too invasive for the surfaced bug; doesn't address the slopes-longer-than-feature_names case at all. |
| OSS-Fuzz onboarding before fixing | Catches the broader class of parser bugs. | Doesn't close the immediate OOB; weeks of integration work. | Orthogonal — already tracked separately for the libvmaf+ai surface. |

## Consequences

- **Positive**: Heap-buffer-overflow read in `vmaf_model_destroy` is
  eliminated. The reproducer in PR #371 now exits cleanly with
  `-EINVAL` and no ASan report. Malformed models are rejected at the
  parse boundary, surfacing the contract violation to operators
  immediately rather than at destroy / predict time. Future regressions
  that drift `n_features` past `feature_cap` cannot become OOB reads in
  the destructor.
- **Negative**: A new error class — JSON models whose per-feature arrays
  disagree in length now fail to load. No shipped Netflix or
  fork-internal model has this shape (verified against all
  `model/*.json` and the K150K + transNet sidecar models).
- **Neutral / follow-ups**:
  - 3 new unit tests in `core/test/test_model.c` cover the
    slopes-longer, intercepts-longer, and slopes-before-names cases.
  - The cross-key validator (`validate_feature_arrays`) is the natural
    home for any future per-feature schema checks (e.g. enforcing
    `feature_opts_dicts` length match instead of allowing it to be
    shorter than `feature_names`).
  - Once PR #371 lands, its
    `core/test/fuzz/json_model_known_crashes/slopes_oob_destroy.bin`
    seed becomes the permanent regression input for this defect.

## References

- PR #371 (`feat(fuzz): add fuzz_json_model + fuzz_dnn_sidecar harnesses`)
  — surfaced the bug; ships the reproducer bin and tracking row.
- [`docs/state.md`](../state.md) row
  `T-JSON-MODEL-SLOPES-FEATURE-CAP-OOB-2026-05-30` — open-bug entry
  closed by this ADR.
- [ADR-0882](0882-fuzz-target-audit-json-model-sidecar.md) — the fuzz
  harness audit that produced the trigger.
- [ADR-0404](0404-nightly-fuzz-triage-keep-gates.md) — green-or-honest
  fuzz CI policy this fix protects.
- CERT C MEM35-C / NASA Power of 10 rule 7 — input-validation rationale.
- Source: `req` ("FIX the heap-buffer-overflow in `vmaf_model_destroy`
  discovered by the fuzzer in PR #371" — paraphrased; user directed the
  parse-time-validation-first approach with destroy as belt-and-suspenders).
