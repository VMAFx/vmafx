<!-- markdownlint-disable MD013 MD060 -->
# ADR-0480: Bootstrap Score Name-Builder Deduplication

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: refactor, predict, libvmaf

## Context

Two functions in the libvmaf core engine each construct the same four bootstrap
score-name strings (suffix `_bagging`, `_stddev`, `_ci_p95_lo`, `_ci_p95_hi`)
appended to a model-collection name:

- `bootstrap_append_named_scores()` in `core/src/predict.c` (per-frame path)
- `vmaf_score_pooled_model_collection()` in `core/src/libvmaf.c` (pooling path)

Both functions duplicate the same literal suffix strings and the same name-buffer
size formula (`strlen(name) + strlen(longest_suffix) + 1`). Both left `//TODO:
dedupe` comments referencing each other. The duplication was noted during the
TODO/FIXME audit of 2026-05-16.

Deduplication carries a real correctness benefit: if a new suffix is ever added or
an existing one is renamed, both call sites must be updated. A single shared header
closes that divergence window.

## Decision

Extract the four suffix string constants and the `BOOTSTRAP_NAME_BUF_SZ()` macro
into a new internal-only header `core/src/bootstrap_names.h`. Both `predict.c`
and `libvmaf.c` include this header; the TODO comments are replaced with a citation
to this ADR. The underlying loops cannot be merged further because the callee
functions differ (`vmaf_feature_collector_append` vs `vmaf_feature_score_pooled`);
introducing a function-pointer indirection for two call sites would add more
complexity than it removes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Full helper function (function-pointer callback) | Single loop body | Adds `typedef`/callback indirection; reduces readability; more code than it replaces | Complexity exceeds benefit for two call sites |
| Leave TODOs as-is | Zero change | Divergence risk grows as new suffixes are added | Does not close the audit finding |
| Inline the suffix constants as named `#define` in each `.c` | No new file | Still duplicated across two translation units | Does not close the divergence window |

## Consequences

- **Positive**: suffix constants are now a single source of truth; the
  `BOOTSTRAP_NAME_BUF_SZ()` formula is defined once; the two `//TODO` markers
  are removed.
- **Negative**: one new internal header file to maintain.
- **Neutral**: no ABI or behaviour change; no test regeneration required.

## References

- `core/src/bootstrap_names.h` (new file)
- `core/src/predict.c` — `bootstrap_append_named_scores()`
- `core/src/libvmaf.c` — `vmaf_score_pooled_model_collection()`
- TODO/FIXME audit: `.workingdir/audit-todo-fixme-2026-05-16.md` items #1 and #2
