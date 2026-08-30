<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1048: vmaf-tune `ladder --duration` sentinel dest mismatch fix

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `vmaf-tune`, `bug`, `cli`

## Context

`_stamp_tracked_default_sentinels` in `cli.py` iterates a hard-coded tuple of
`dest` names to decide which flags were left at their default value. The `ladder`
sub-command registers `--duration` with an explicit `dest="duration_s"` (to
avoid colliding with the `corpus` sub-command's `args.duration`). However,
`"duration_s"` was not in the sentinel tuple — only `"duration"` was. As a
result, `_duration_s_was_default` was never set for `ladder` invocations, making
the ladder source-duration always appear as user-overridden regardless of whether
the user actually passed the flag. Downstream bitrate-math that branched on
`_duration_s_was_default` would silently use the wrong code path.

## Decision

Add `"duration_s"` to the sentinel tuple in `_stamp_tracked_default_sentinels`
alongside the existing `"duration"` entry. Both entries are now present so the
stamp pass covers both the `corpus` sub-command (dest=`duration`) and the
`ladder` sub-command (dest=`duration_s`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Normalise `ladder --duration` dest to `"duration"` | Removes the divergence | Would require auditing every downstream read of `args.duration` vs `args.duration_s` across the two sub-commands — high risk of introducing a new cross-contamination bug | Not chosen in this PR; the sentinel fix is safe and minimal |
| Use `_TrackedDefaultAction` on ladder `--duration` | Self-describing | Requires changing the `_stamp_tracked_default_sentinels` contract anyway | Equivalent outcome; minimal change preferred |

## Consequences

- **Positive**: `ladder --duration` default-detection now works correctly;
  bitrate math uses the right branch when the user does not pass `--duration`.
- **Negative**: None.
- **Neutral / follow-ups**: If additional `_TrackedDefaultAction` flags are added
  with non-standard `dest=` values in the future, the comment in
  `_stamp_tracked_default_sentinels` now documents the pattern.

## References

- R9 bug hunt report finding: `cli.py:147` (2026-06-04).
- `tools/vmaf-tune/src/vmaftune/cli.py`.
