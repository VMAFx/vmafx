<!-- markdownlint-disable MD013 MD060 -->
# ADR-0536: Per-shot predicate threads bitrate_kbps through bisect sidecar (PR #1290 follow-up)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vmaf-tune`, `per-shot`, `bugfix`

## Context

ADR-0531 (PR #1290) specified the correct fix for the per-shot bitrate column
producing `null` for all shots: introduce a `bitrate_sidecar` dict inside
`_build_per_shot_bisect_predicate`, populate it from `result.bitrate_kbps` as
each shot's bisect completes, and annotate each `ShotRecommendation` via
`dataclasses.replace` after `tune_per_shot` returns.

The ADR-0531 PR added the `ShotRecommendation.bitrate_kbps` field and the
`_shot_bitrate` null-serialiser but the implementation of
`_build_per_shot_bisect_predicate` was left incomplete: the function still
returned only `_predicate` (not the `(predicate, sidecar)` pair) and the
`_predicate` closure discarded `result.bitrate_kbps` before returning
`(result.best_crf, result.measured_vmaf)`. The BBB v11 agent confirmed all 12
shots in the plan JSON still showed `bitrate_kbps: null` after PR #1290 merged.

## Decision

Implement the bitrate-sidecar wiring described in ADR-0531:

1. Change `_build_per_shot_bisect_predicate` to return
   `tuple[PerShotPredicateFn, dict[tuple[int, int], float]]`.
2. The inner `_predicate` closure stores `result.bitrate_kbps` into the sidecar
   dict keyed by `(shot.start_frame, shot.end_frame)` before returning.
3. The call site in `_run_tune_per_shot` unpacks the tuple, initialises an empty
   `bitrate_sidecar` for the `--predicate-module` path, and after `tune_per_shot`
   returns patches each `ShotRecommendation` via `dataclasses.replace` to inject
   the measured bitrate from the sidecar (defaulting to the NaN field value for
   shots absent from the sidecar).

The `PredicateFn` public type alias remains `Callable[[Shot, float, str],
tuple[int, float]]` — no blast radius on custom predicates or library callers.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Widen `PredicateFn` to `tuple[int, float, float]` | No side-channel | Breaks all existing `--predicate-module` callers and the `_default_predicate` stub | Breaking API change — ruled out in ADR-0531 |
| Re-encode each shot after `tune_per_shot` to measure bitrate | Bit-exact final-segment bitrate | Doubles encode time; bisect already has the best-CRF encode | Unnecessary work |
| Sidecar dict keyed by shot index | Avoids tuple key | Fragile if `tune_per_shot` ever re-orders or skips shots | Key on `(start_frame, end_frame)` is content-stable |

## Consequences

- **Positive**: `bitrate_kbps` in the per-shot plan JSON carries real kbps
  values from the bisect backend; the BBB v11 finding is resolved.
- **Negative**: `_build_per_shot_bisect_predicate` return type changed from
  `PerShotPredicateFn` to a 2-tuple — any fork-external caller that assigned
  the return to a `PerShotPredicateFn`-typed variable must be updated (none
  exist in tree).
- **Neutral**: external `--predicate-module` predicates still yield `null`
  bitrate (sidecar remains empty); this is correct — no real encode happened.

## References

- ADR-0531: original specification of the sidecar pattern (PR #1290 incomplete).
- BBB v11 audit report: Finding 10, "Per-shot bitrate column — FAIL".
- `tools/vmaf-tune/src/vmaftune/cli.py` — `_build_per_shot_bisect_predicate`,
  `_run_tune_per_shot`.
- `tools/vmaf-tune/src/vmaftune/per_shot.py` — `ShotRecommendation`,
  `PredicateFn`.
- `tools/vmaf-tune/tests/test_per_shot.py` — regression test
  `test_cli_tune_per_shot_bitrate_kbps_propagates_from_bisect`.
