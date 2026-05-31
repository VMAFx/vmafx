<!-- markdownlint-disable MD060 -->
# ADR-0508: vmaf-tune ladder pass-1 stats argv honours --duration

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, claude
- **Tags**: vmaf-tune, ladder, encode, bugfix

## Context

The BBB end-to-end v8 probe (after PR #1263 / ADR-0506 closed the
v6 cluster's V6-1 "metadata-only `--duration`" bug) surfaced a sibling
of V6-1 that the V6-1 fix did not reach:

- **V8-A** — `vmaf-tune ladder --src bbb.mp4 --duration 5 ...` against
  any codec adapter that declares `supports_encoder_stats = True`
  (`libx264` in practice — the corpus default) still re-encoded the
  full source during the ffmpeg **pass-1 stats sweep**. V6-1 patched
  `build_ffmpeg_command` to honour `EncodeRequest.duration_s` as an
  input-side `-t` when the caller didn't opt into sample-clip mode,
  and that fix covered single-pass `run_encode` plus 2-pass
  `run_two_pass_encode` (both of which compose argv via
  `build_ffmpeg_command`). But `run_encode_with_stats` — the path
  the corpus driver routes libx264 encodes through to capture the
  per-frame stats file — composes argv via a sibling builder
  `build_pass1_stats_command` that was *not* updated, so its
  pass-1 invocation lacked `-t duration_s` even when the caller
  bound `--duration N`. Pass 2 then ran via `run_encode` and
  *did* honour the window, but the pass-1 stats sweep had already
  burned >60× the requested wall time on a 10-minute source.

The diagnostic ffmpeg command observed in the v8 probe:

```text
ffmpeg -y -hide_banner -loglevel info -i bbb_..._normal.mp4 \
  -c:v libx264 -preset medium -crf 23 -vf scale=1920:1080 \
  -pass 1 -passlogfile /tmp/vmaftune_stats_..._crf23 -f null /dev/null
```

— note the missing `-t 5` between `-loglevel info` and `-i`.

## Decision

Mirror the V6-1 fallback in
`vmaftune.encode.build_pass1_stats_command`: when the caller bound
`req.duration_s > 0` without opting into sample-clip mode, emit
`-t req.duration_s` on the input side (before `-i`) just like
`build_ffmpeg_command` already does. Precedence matches V6-1 — when
`sample_clip_seconds > 0` the explicit `-ss / -t sample_clip_seconds`
argv wins and the `duration_s` fallback stays out of the argv so the
two flags never double up.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Mirror V6-1 fallback in `build_pass1_stats_command` (chosen) | Minimal diff, identical semantics to V6-1, easy to test against `build_ffmpeg_command` symmetry | One more place to keep in sync the next time clip semantics evolve | Picked — smallest fix surface, V6-1 invariant preserved |
| Refactor pass-1 to call `build_ffmpeg_command` with `pass_number=1` | Single source of truth for input-side clipping | Pass-1 stats argv has a different `-pass 1 -passlogfile … -f null /dev/null` tail; the unification would require restructuring `build_ffmpeg_command` to know about a stats-only mode (or wrapping it) — larger blast radius mid-merge-train | Deferred; can land later as a refactor without changing observable behaviour |
| Drop `supports_encoder_stats` from libx264 when `duration_s > 0` | Sidesteps the bug entirely | Loses the ADR-0332 per-frame stats capture that downstream RC analysis depends on, just because the user asked for a shorter window | Rejected — the stats capture is the point, not a side effect |

## Consequences

- **Positive**: `vmaf-tune ladder --duration N` now actually clips the
  entire encode pipeline (pass 1 + pass 2 + reference decode); a
  smoke run scales with `N`, not source length. Closes the V8-A
  symptom: a 5-second probe against 10-min BBB now takes seconds
  per cell instead of minutes.
- **Negative**: One more argv path to keep mirrored with
  `build_ffmpeg_command`'s input-side clipping logic. The new test
  file `test_bbb_e2e_v8_bug_cluster.py` pins the contract on both
  sides; a future divergence will be caught.
- **Neutral / follow-ups**: None — the fix is local to one
  argv-builder. If the deferred refactor (collapse
  `build_pass1_stats_command` into `build_ffmpeg_command`) lands
  later, the V8-A tests in the new file remain valid.

## References

- PR #TBD (V8-A fix in this ADR)
- PR #1263 / ADR-0506 — V6-1 fix in `build_ffmpeg_command`
- ADR-0332 — `supports_encoder_stats` rationale (per-frame x264
  stats capture for RC analysis)
- ADR-0333 — Phase F 2-pass encoding (`run_two_pass_encode`
  uses `build_ffmpeg_command`, not the stats helper)
- `tools/vmaf-tune/src/vmaftune/encode.py::build_pass1_stats_command`
- `tools/vmaf-tune/tests/test_bbb_e2e_v8_bug_cluster.py`
- Source: `req` — task brief "Bug V8-A: ladder code path missed
  `-t 10` plumbing that PR #1263 added for tune-per-shot"
