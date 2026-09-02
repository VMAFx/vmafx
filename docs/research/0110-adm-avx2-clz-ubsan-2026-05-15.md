# Research-0110: ADM AVX2 direct-LUT UBSan fix

- **Status**: implementation digest
- **Date**: 2026-05-15
- **Relevant state row**: T-SANITIZER-SCORE-POOLED-EAGAIN-CLEAN

## Question

Why did `test_score_pooled_eagain` fail only in the UBSan lane after it
was removed from the sanitizer deselect list?

## Findings

- The failure was not in the score-pooling EAGAIN path. UBSan reached
  `adm_decouple_s123_avx2` and reported `__builtin_clz(0)` in
  `get_best15_from32`.
- The scalar ADM path only calls `get_best15_from32` for values
  `>= 32768`; smaller absolute values go through the direct lookup
  table with shift `0`.
- The AVX2 path called the helper for every lane before blending the
  direct-LUT result back in. That made zero and other values below
  `32768` invalid inputs to the helper even though their reduced result
  should not be used.

## Decision Support

| Option | Trade-off | Decision |
| --- | --- | --- |
| Re-add `test_score_pooled_eagain` to the UBSan deselect list | Restores CI quickly but hides a real AVX2 ADM undefined-behaviour path. | Rejected |
| Special-case only `temp == 0` | Fixes the observed `clz(0)`, but leaves negative shift counts for other direct-LUT-range values. | Rejected |
| Make `get_best15_from32` total for `temp < 32768` | Matches scalar ADM semantics: direct-LUT value plus shift `0`; larger values keep the existing rounded reduction. | Accepted |

## Non-Goals

- No change to Netflix golden assertions or expected scores.
- No changes to the remaining sanitizer deselect entries.
