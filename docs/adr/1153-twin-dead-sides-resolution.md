<!-- markdownlint-disable MD013 MD060 -->
# ADR-1153: Resolution of Dead .c/.cpp Twin Sides (model.cpp, test_dict.c, test_feature.c)

- **Status**: Accepted
- **Date**: 2026-09-03
- **Deciders**: Lusoris
- **Tags**: `build`, `ci`, `refactor`, `fork-local`

## Context

The [ADR-1135](1135-ci-twin-drift-gate.md) twin-drift and stale-source-reference gate surfaced
three translation units that were compiled by no build file (`meson.build`, `setup.py`, `*.pyx`):

1. `core/src/model.cpp`: Created during [ADR-0729](0729-cpp23-wave3-bundle.md) (Wave 3) as a
   C++23 twin of `model.c`. PR #1133 wired twelve C++23 Wave 1-5 twins into the build, but omitted
   `model.cpp`. Consequently, `core/src/meson.build` continued compiling `model.c`, leaving
   `model.cpp` orphaned and uncompiled while fixes accumulated on `model.c`.
2. `core/test/test_dict.c`: Pre-ADR-0729 C test twin that text-included `"dict.c"`. When `dict.c`
   was converted and renamed to `dict.cpp` in PR #1133, `test_dict.c` was left unable to compile.
   Meanwhile, `core/test/test_dict.cpp` was already compiled and registered in `core/test/meson.build`.
3. `core/test/test_feature.c`: Pre-ADR-0729 C test twin that text-included `"feature/feature_name.c"`.
   When `feature_name.c` was converted and renamed to `feature_name.cpp` in PR #1133, `test_feature.c`
   was also left unable to compile. `core/test/test_feature.cpp` was already compiled and registered
   in `core/test/meson.build`.

All three files were temporarily listed in `scripts/ci/twin-drift-allowlist.txt` under tracking task
`T-TWIN-DEAD-SIDES-2026-09-02` in `docs/state.md`.

## Decision

We delete all three dead twin files (`core/src/model.cpp`, `core/test/test_dict.c`, and
`core/test/test_feature.c`), preserve `core/src/model.c` as the sole authoritative model implementation,
rely on the surviving `core/test/test_dict.cpp` and `core/test/test_feature.cpp` test suites, and remove
their entries from `scripts/ci/twin-drift-allowlist.txt`, reducing the allowlist to zero dead sides.

Detailed rationale per file:

- **`core/src/model.cpp`**: A function-by-function diff against `core/src/model.c` confirmed that
  `model.cpp` is severely stale and incomplete:
  1. It is missing the 8 upstream VMAF v1.0.16 SDR models ported in PR #1024 (Netflix commit `4718b4f5f`);
  2. It is missing the `pthread_mutex_destroy(&model->predict_cache_lock)` cleanup in `vmaf_model_destroy`
     from the Round-5 race fix (PR #864);
  3. It contains a heap-buffer-overflow defect in `vmaf_model_destroy` (iterating up to `max(feature_cap, n_features)`
     instead of `min(feature_cap, n_features)`), which was fixed in `model.c` under ADR-0887 / PR #743;
  4. It carries outdated log severity (`ERROR` instead of `WARNING` in `vmaf_model_load_from_path`, demoted in PR #858);
  5. Its promised RAII `ModelCollectionGuard` was never implemented (it uses manual cleanup with duplicate code).
  Wiring `model.cpp` into the build would re-introduce multiple bugs and break upstream feature parity.
  Deleting `model.cpp` preserves the verified, working `model.c`.
- **`core/test/test_dict.c`**: Deleted as obsolete. Its compiled C++ twin `core/test/test_dict.cpp`
  already covers 100% of its test surface across 7 test functions (`test_vmaf_dictionary`,
  `test_vmaf_dictionary_merge`, `test_vmaf_dictionary_compare`, `test_vmaf_dictionary_normalize_numerical_val`,
  `test_vmaf_feature_dictionary`, `test_vmaf_dictionary_alphabetical_sort`, `test_isnumeric`), with identical
  assertions, avoiding ODR violations via `dict_internal.h`.
- **`core/test/test_feature.c`**: Deleted as obsolete. Its compiled C++ twin `core/test/test_feature.cpp`
  is active in `core/test/meson.build` and covers `vmaf_feature_name_from_options`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Wire `model.cpp` as a `*_cpp23_lib`** | Advances C++23 migration | `model.cpp` is stale, missing 8 built-in models, missing mutex destruction, and contains a heap-buffer-overflow regression | Rejected — would break functionality and upstream parity for no operational gain. |
| **Delete `model.cpp` and keep `model.c`** | Zero regressions; preserves working models and golden correctness; zero LOC drift | `model.c` remains C rather than C++23 | **Chosen** — `model.c` is verified, passes all tests, and matches upstream. |
| **Port `test_dict.c` to include `dict.cpp`** | Retains a C test file | `test_dict.cpp` already exists and tests the identical assertions in the build | Rejected — redundant maintenance overhead. |
| **Delete `test_dict.c`** | Removes dead file that cannot compile | None | **Chosen** — `test_dict.cpp` is already the surviving equivalent. |
| **Port `test_feature.c` to include `feature_name.cpp`** | Retains a C test file | `test_feature.cpp` already exists and tests the feature name generation in the build | Rejected — redundant maintenance overhead. |
| **Delete `test_feature.c`** | Removes dead file that cannot compile | None | **Chosen** — `test_feature.cpp` is already the surviving equivalent. |
| **Leave allowlisted in `twin-drift-allowlist.txt`** | Zero code changes | Perpetuates dead code, maintenance hazards, and allowlist clutter | Rejected runner-up — the allowlist is a temporary triage mechanism, not a permanent home for abandoned code. |

## Consequences

- **Positive**: `scripts/ci/twin-drift-allowlist.txt` shrinks from 3 dead sides to 0; `scripts/ci/twin-drift-check.sh` passes cleanly with 0 dead sides; eliminating silent drift and uncompiled code.
- **Negative**: None.
- **Neutral / follow-ups**: Close task `T-TWIN-DEAD-SIDES-2026-09-02` in `docs/state.md`. Update `core/AGENTS.md` and `core/test/AGENTS.md` with invariant notes stating that `model.c`, `test_dict.cpp`, and `test_feature.cpp` are the authoritative files.

## References

- [ADR-1135](1135-ci-twin-drift-gate.md) — CI twin-drift + stale-source-reference gate
- [ADR-0729](0729-cpp23-wave3-bundle.md) — C++23 Wave 3 bundle
- [ADR-0887](0887-json-model-array-validation.md) — model destroy bounds check
- PR #1024 — port upstream VMAF v1.0.16 SDR models
- PR #1133 — wire C++23 Wave 1-5 twins
- Task row `T-TWIN-DEAD-SIDES-2026-09-02` in `docs/state.md`
- Source: `req` — user prompt to close T-TWIN-DEAD-SIDES-2026-09-02
