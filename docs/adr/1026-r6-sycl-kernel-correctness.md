# ADR-1026: R6 SYCL kernel correctness — rd-stride OOB and unchecked graph_wait

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `correctness`, `sycl`

## Context

Round-6 analysis found two classes of correctness bugs in the SYCL feature
extractor paths:

1. **VIF rd-downsample stride truncation for odd widths** (two locations):
   `integer_vif_sycl.cpp` lines 822 and 1313 use `e_w / 2` as the rd-buffer
   row stride.  When `e_w` is odd (e.g. 1279 pixels), integer division
   truncates: `1279 / 2 = 639`, but the downsampled row has `(1279 + 1) / 2
   = 640` pixels.  The last active thread on that row writes to
   `rd_ref[gy/2 * 639 + 639]` which is one element into the next row,
   corrupting the adjacent row's data.  The allocation uses the rounded-up
   formula `(w+1)/2 * (h+1)/2`; the stride must match.

2. **Unchecked `vmaf_sycl_graph_wait()` return values** (three locations):
   `integer_motion_sycl.cpp:610`, `integer_vif_sycl.cpp:1635`, and
   `integer_adm_sycl.cpp:1559` all call `vmaf_sycl_graph_wait()` (which
   returns `int`) and discard the result.  If the SYCL queue throws
   (device fault or reset), the wait returns a non-zero error code; the
   extractor then reads stale accumulator memory and emits a wrong score —
   silently, with no error propagation to the caller.

## Decision

- Replace `e_w / 2` with `(e_w + 1u) / 2u` for the rd stride in both VIF
  SYCL variants (standard SIMD and SIMD-16).
- Capture the `vmaf_sycl_graph_wait()` return value in all three collect
  functions and propagate it as an early return.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Assert `e_w % 2 == 0` at launch time | Catches odd widths earlier | Rejects valid odd-width content | VIF must handle arbitrary resolutions |

## Consequences

- **Positive**: SYCL VIF scores are correct for odd-width inputs; device-fault
  errors on the SYCL path are now propagated to the scoring pipeline instead
  of silently producing stale scores.
- **Negative**: A device fault now surfaces as a scoring error rather than a
  stale-but-numeric result.  Callers that did not check the `collect` return
  value before will now see non-zero returns.
- **Neutral**: No change to the score values for even-width inputs.

## References

- Round-6 scanner labels: `r6-sycl-kernel` x 5
- `core/src/feature/sycl/integer_vif_sycl.cpp`
- `core/src/feature/sycl/integer_motion_sycl.cpp`
- `core/src/feature/sycl/integer_adm_sycl.cpp`
