<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1100: Skip GPU-flagged extractors when `flags == 0` in `vmaf_get_feature_extractor_by_feature_name`

- **Status**: Accepted
- **Date**: 2026-06-07
- **Deciders**: Lusoris
- **Tags**: `feature-extractor`, `correctness`, `sycl`, `cuda`, `hip`, `bug-fix`, `fork-local`

## Context

`vmaf_use_features_from_model` calls `vmaf_get_feature_extractor_by_feature_name(name, flags=0)`
when no GPU context is present (CPU-only caller path). The first-pass filter inside that function
was:

```c
if (flags && !(fex->flags & flags)) continue;
```

When `flags == 0`, the subexpression `flags && ...` is always false (0 short-circuits), so the
filter was a no-op: every extractor in `feature_extractor_list` became a candidate regardless of
its own flags. In an all-backends build (CUDA + SYCL + HIP all compiled in), GPU-flagged twins
appear before their CPU counterparts in `feature_extractor_list` because they are registered in
the GPU backend source files which are compiled first. Consequently, `vmaf_get_feature_extractor_by_feature_name("adm", 0)`
returned `integer_adm_sycl` instead of `integer_adm` (CPU).

The SYCL extractor's `init` function guards against missing SYCL state:

```c
if (!fex->sycl_state) return -EINVAL;  // integer_adm_sycl.cpp:1301
```

With no GPU context, this guard fired on every frame, causing `vmaf_read_pictures` to return
`-EINVAL` on every frame with the message "problem during vmaf_read_pictures". The CPU scoring
path was completely bypassed.

This bug was latent in CPU-only builds because GPU extractors are not compiled in; it became
visible as soon as CUDA or SYCL was included in the same binary alongside CPU-fallback usage.

## Decision

When `flags == 0`, the first-pass loop in `vmaf_get_feature_extractor_by_feature_name` now skips
any extractor whose `flags` field has any of `VMAF_FEATURE_EXTRACTOR_CUDA`,
`VMAF_FEATURE_EXTRACTOR_SYCL`, or `VMAF_FEATURE_EXTRACTOR_HIP` set. CPU-only extractors (flags
== 0) are unaffected and are selected normally. When `flags != 0`, the existing exact-match
semantics are preserved unchanged, as is the ADR-0530 fallback second pass (only entered when
`flags != 0`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Reorder `feature_extractor_list` to put CPU twins first | No code-logic change | Fragile: source-file compile order determines registration order; any future GPU extractor added before its CPU twin silently re-introduces the bug | Brittle; mask of the actual problem |
| Add `flags == 0` guard to each GPU extractor's `init()` | Bug contained at init level | Every future GPU extractor needs the same boilerplate; hard to audit | Decentralised fix; doesn't address the root cause in the lookup |
| Only fix in second pass (fallback) | Minimal change | First pass still returns GPU extractor when `flags == 0`; the SYCL init guard causes -EINVAL before we ever reach fallback | Wrong: second pass is only entered when `flags != 0` |
| This fix: `flags == 0` → skip `gpu_mask` extractors in first pass | Correct, centralised, minimal | None | Chosen |

## Consequences

- **Positive**: CPU-only callers consistently receive CPU twins even in all-backends builds.
  `vmaf_read_pictures` works correctly without a GPU context. The fix is three lines of logic
  change at one callsite.
- **Negative**: None — the second-pass ADR-0530 fallback (for `flags != 0`) is unchanged.
- **Neutral / follow-ups**: The pre-existing `<string.h>` omission in `core/test/test_picture.c`
  (implicit `memset` declaration, caught by this build pass) fixed in the same PR.

## References

- ADR-0530: HIP fallback second pass in `vmaf_get_feature_extractor_by_feature_name`
- Root-cause identification: workflow `wf_13f4ebec-eb7-1` (2026-06-07)
- Reproducer: build with `-Denable_cuda=false -Denable_sycl=true` (or `-Denable_cuda=true`) +
  call `vmaf_use_features_from_model` without initialising GPU state; observe `-EINVAL` on every
  `vmaf_read_pictures` frame.
