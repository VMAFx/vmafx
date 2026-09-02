# ADR-0806: VmafFeatureDictionary caller-ownership contract

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `api`, `memory`, `testing`

## Context

`vmaf_use_feature()` unconditionally takes ownership of the
`VmafFeatureDictionary *opts_dict` argument and frees it internally —
both on success and on failure — via `vmaf_dictionary_free(&s)` in
`core/src/libvmaf.c` (lines 1520-1525). `vmaf_model_feature_overload()`
follows the same pattern: it always frees its `opts_dict` argument via
`vmaf_dictionary_free((VmafDictionary **)&opts_dict)` at the end of the
function (model.c line 196).

A PR #126 Doxygen audit surfaced the ownership semantics as unclear;
subsequent review found two concrete bugs in test code:

1. **Double-free (CWE-415)** — `core/test/test_vif_skip_scale0.c` called
   `vmaf_feature_dictionary_free(&opts)` on an error path *after*
   `vmaf_use_feature` had already freed `opts`. Because
   `vmaf_use_feature` frees `opts` regardless of whether it returns
   success or an error code, the error-path explicit free is always
   a double-free.

2. **Leak (CWE-401)** — `core/test/test_integer_vif_cpu_cuda_parity.c`
   allocated `chroma_opts` and passed it to `run_cuda_vif()`. That
   helper returns early (before calling `vmaf_use_feature`) when no
   CUDA device is present, leaving `chroma_opts` allocated but
   unreachable.

No production (non-test) callers were affected; `cli_parse.c` and its
companion fuzz driver handle cleanup via their own
`CHECKED_APPEND` / `free_settings_dicts` patterns.

## Decision

The contract is codified in `core/include/libvmaf/feature.h` (existing
doc-comment on `vmaf_feature_dictionary_set`): callers must NOT free a
dict after passing it to `vmaf_use_feature()` or
`vmaf_model_feature_overload()` — those functions take ownership.
Functions that return early without calling an ownership-transferring
function are responsible for freeing any dict they hold.

Fixes applied:

- `test_vif_skip_scale0.c`: removed the double-free on the error path;
  added a `opts = NULL` assignment after the call to document transfer,
  and a comment citing this ADR.
- `test_integer_vif_cpu_cuda_parity.c`: added
  `vmaf_feature_dictionary_free(&opts)` on the early-return (no CUDA
  device) path in `run_cuda_vif()`, citing this ADR.

## Alternatives considered

**Copy the dict in `vmaf_use_feature` (caller retains ownership)** —
simpler call-site reasoning, but this is an ABI break and existing
correct callers would then leak. Rejected: backwards-incompatible.

**Add a `vmaf_use_feature_no_transfer` variant** — lets callers choose,
but adds API sprawl; current callers are fine with transfer semantics.
Rejected: premature generality.

## Consequences

- **Positive**: double-free and leak eliminated from the test suite;
  ownership contract is now explicit in both header docs and an ADR.
- **Negative**: none.
- **Neutral / follow-ups**: any future caller of `vmaf_use_feature` or
  `vmaf_model_feature_overload` must follow the transfer contract
  documented in `feature.h`.

## References

- PR #126 Doxygen audit (triggered this audit).
- `core/include/libvmaf/feature.h` — ownership doc-comment on
  `vmaf_feature_dictionary_set`.
- `core/src/libvmaf.c:1520-1525` — transfer + free in
  `vmaf_use_feature`.
- `core/src/model.c:196` — transfer + free in
  `vmaf_model_feature_overload`.
- Source: user request (paraphrased: audit VmafFeatureDictionary callers
  for leak, double-free, and use-after-free; apply fixes; open ready PR
  with auto-merge).
