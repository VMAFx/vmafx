<!-- markdownlint-disable MD013 MD060 -->
# ADR-0729: C++23 Wave 3 — feature_name, picture_copy, model

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `build`, `cpp23`, `refactor`

## Context

ADR-0708 established the C++23 migration playbook (isolated `static_library` +
`override_options = ['cpp_std=c++23']`) and converted `metadata_handler.c`.
Wave 1 PRs (#41 mem, #43 opt, #44 fex_ctx_vector, #45 log) and Wave 2 PR #48
(project-wide `cpp_std=c++23` + dict) are queued. Wave 3 continues the series,
targeting three low-risk, low-LOC translation units that each carry meaningful
RAII or type-safety opportunities:

- `feature/feature_name.c` (190 LOC): two `goto`-cleanup patterns over
  `VmafDictionary*`; prime candidate for `std::unique_ptr` + `DictDeleter`.
- `feature/picture_copy.c` (62 LOC): raw pointer arithmetic, C-style casts;
  `std::span` makes per-row bounds explicit.
- `model.c` (368 LOC): multi-label `goto` in `vmaf_model_collection_append`,
  `malloc`+`memset` pairs, C-style casts throughout.

## Decision

Rename the three `.c` files to `.cpp`, apply C++23 idioms (`std::unique_ptr`,
`std::span`, `[[nodiscard]]`, `static_cast<>`, `nullptr`), add `extern "C"`
guards to all affected internal headers, and wire each into the build as an
isolated `static_library` with `override_options = ['cpp_std=c++23']`.
Test files that unity-included the `.c` sources are renamed to `.cpp`
accordingly. The `override_options` entry is portable across both pre- and
post-PR-#48 build states.

A use-after-free was discovered and fixed during conversion of
`feature_name.cpp`: the original `goto`-based cleanup was safe because
`vmaf_dictionary_set` may reallocate the dict pointer, but the naive RAII
translation (`opts_guard.reset(opts_raw)` inside the loop) freed the previous
value while `vmaf_dictionary_set` had already reused it. The fix: delay RAII
adoption until after the loop, with explicit `vmaf_dictionary_free` on the
error path.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-file PRs (one PR per .c → .cpp) | Smallest possible diff, easiest review | Four round-trips through the merge train; high overhead relative to the change | Bundle is faster and the files are independent |
| Keep `override_options` until PR #48 lands, then patch | Cleaner diff post-PR-#48 | PR #48 may not land before this branch merges; would leave the build broken on the current master | `override_options` is harmless once the project-wide flag is active |
| Skip `model.c` (>300 LOC) | Smaller bundle | model.c has concrete RAII opportunities (multi-label goto) and its test already unity-includes the source; converting together avoids a split transition | Included; test renamed to `.cpp` in the same commit |

## Consequences

- **Positive**: three more TUs gain RAII cleanup guarantees; latent UAF in
  `feature_name.c` (double-free on dict realloc under RAII) is fixed;
  internal headers (`log.h`, `read_json_model.h`, `opt.h`, `alias.h`,
  `feature_extractor.h`) gain `extern "C"` guards, reducing friction for
  future C++ consumers.
- **Negative**: test files `test_feature.c` and `test_model.c` are renamed to
  `.cpp` (pre-existing unity-include pattern requires the test to be compiled
  as C++); this is a one-time cost per file.
- **Neutral**: `picture_copy.h` gains include guards it was previously missing.

## References

- ADR-0708 (C++23 migration playbook)
- Wave 1 PRs: #41, #43, #44, #45; Wave 2 PR: #48
- Per user direction: "Convert cpp23 Wave 3 candidates — feature_name, picture_copy, model"
