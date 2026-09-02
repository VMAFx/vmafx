<!-- markdownlint-disable MD060 -->
# ADR-0509: vmaf-tune compare auto-probes container-source framerate / duration

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, claude
- **Tags**: vmaf-tune, compare, bisect, encode, vmaf-cli

## Context

The BBB end-to-end v7 probe surfaced a sister bug to ADR-0505: the
`vmaf-tune compare` CLI silently returned catastrophically wrong
VMAF scores when `--src` was a container (mp4 / mkv / mov / ...).
Reproducer (against the dev-mcp BBB MP4 at native 60 fps):

```text
vmaf-tune compare \
  --src /workspace/.corpus/bbb_e2e/bbb_sunflower_1080p_60fps_normal.mp4 \
  --width 1920 --height 1080 --target-vmaf 92 \
  --encoders libx264,libx265 --duration 5 --sample-clip-seconds 3 \
  --max-iterations 3 --score-backend cuda --format json
```

Both encoders converged to `ok=false`: `libx264` "closest miss
VMAF=90.43 at CRF=6" (CRF 6 at 1080p is near-lossless and must score
>= 98); `libx265` capped at VMAF=90.05 at CRF=18. The numbers are
not just low — they are physically impossible for the encode
geometry, mirroring the ADR-0505 symptom shape (uniformly bogus VMAF
regardless of CRF on container sources).

Root cause: the compare CLI threaded `--framerate` (argparse default
`24.0`) and `--duration` (default `0.0`) into `make_bisect_predicate`
verbatim. The per-iteration `frame_skip_ref` / `frame_cnt` (computed
in `bisect._sample_clip_window` from the user-supplied framerate)
no longer indexes the same source frames the encoder pulled — the
encoder's input-side `-ss 1.0 -t 3.0` clips a 3-second window of
the container, but ffmpeg then decodes both the reference and the
distorted MKV back to raw YUV at the container's native rate
(60 fps), while `vmaf --frame_skip_ref 24` (= 1.0 s * 24 fps) skips
only 24 frames = 0.4 s of native-rate YUV in the reference,
comparing misaligned content frame-by-frame. The result: a VMAF
collapse to the 4 - 90 band regardless of CRF.

The ADR-0505 / V5-2 fix plumbed `source_is_container=True` through
`CorpusJob -> EncodeRequest -> build_ffmpeg_command` for the ladder
path. The compare path's encode plumbing was already correct
(`bisect._encode_and_score` already builds the `EncodeRequest` with
`source_is_container=src_is_container`); the remaining defect was
that the per-iteration *score-side* frame-window alignment used the
user-supplied `--framerate` which silently disagreed with the
container's native rate.

## Decision

We will auto-probe container sources in `_run_compare` and substitute
the probed framerate / duration when the user left those flags at
their argparse defaults. Explicit user values still win, with a one-
line stderr warning on explicit mismatch.

Implementation:

1. Add `_TrackedDefaultAction` (argparse custom action) and a post-
   `parse_args` `_stamp_tracked_default_sentinels` pass so that
   `_run_compare` can distinguish "user explicitly passed
   `--framerate 24`" from "argparse default 24" — the inverse
   pattern is needed because argparse never invokes `Action.__call__`
   when the user omits the flag.
2. Wire `_TrackedDefaultAction` onto the compare subparser's
   `--framerate` and `--duration` flags.
3. Add `_resolve_compare_source_geometry(src, ..., probe_fn=...)`
   that calls `vmaftune.report.probe_source` on a container source,
   substitutes probed framerate / duration when the user left them
   at defaults, fills in missing `--width` / `--height` from the
   probe, and emits a stderr warning when an explicit user
   `--framerate` disagrees with the probe by more than 0.01 fps.
4. `_run_compare` calls the helper before constructing the
   `make_bisect_predicate` closure; the existing `--width` / `--height`
   mandatory-check moves to the post-probe geometry.

The helper is pure (probe and warn-stream are injectable) so the
regression test stubs both without shelling out to `ffprobe`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Auto-probe + override user values when they mismatch the probe | Single source of truth; eliminates user-error footgun entirely | Breaks legitimate subsampling use-cases ("score this 60fps source as 30fps for a quick smoke run"); silent override is exactly the class of behaviour ADR-0505 was rolled back for | Rejected: probe wins by default but explicit user value must take precedence; warn on mismatch instead |
| Require user to always pass `--framerate` for container sources | Forces operator awareness; no silent probe path | Hostile UX — `ffprobe` is right there and the answer is unambiguous for 99% of sources; doesn't actually fix the bug for callers who pass the wrong rate | Rejected: the default path is the dominant one; usability matters |
| Change `--framerate` default to `None` and require it for raw YUV but auto-fill for containers | Cleaner sentinel semantics (no marker attribute needed) | Breaks every existing caller that relies on the 24.0 default for raw YUV; existing test `test_cli_compare_binds_real_bisect_predicate` already pins the 24.0 behaviour | Rejected: backward-compat for raw YUV trumps sentinel cleanliness |
| Probe inside `make_bisect_predicate` rather than `_run_compare` | Centralises the fix at the bisect entry point so all callers (not just CLI) get it | `make_bisect_predicate` is a pure-Python adapter consumed by programmatic callers who may already have their geometry pinned; an unsolicited probe inside it would surprise tests and library users | Rejected: CLI is the right layer for an ffprobe-touching helper |

## Consequences

- **Positive**:
  - `vmaf-tune compare --src container.mp4 ...` now returns sensible
    VMAFs out of the box on any container the system's `ffprobe`
    understands. The reproducer above now converges at libx264
    CRF=26 (VMAF=94.9, 528 kbps) and libx265 CRF=32 (VMAF=93.6,
    365 kbps).
  - Closes the compare half of the BBB e2e v7 cluster; sister to
    ADR-0505 (ladder) and ADR-0506 (V6-1 ladder duration clamp) /
    ADR-0508 (pass-1 stats duration clamp).
- **Negative**:
  - Adds one `ffprobe` call per `compare` invocation against a
    container source. Cost is negligible (< 100 ms) compared to the
    per-iteration encode + score.
  - The `_TrackedDefaultAction` sentinel pattern is mildly subtle
    — adding more `--<flag>` to the tracked list requires editing
    `_stamp_tracked_default_sentinels` too. The hardcoded tuple is
    intentional (cheap loop) but a future generalisation that
    introspects `parser._actions` would remove the duplication.
- **Neutral / follow-ups**:
  - The same auto-probe pattern could be lifted to `vmaf-tune
    ladder` and `vmaf-tune tune-per-shot` for symmetry — neither
    currently has the silent-misalignment failure in the same
    shape (ladder relies on `CorpusJob.framerate` set explicitly
    by the corpus builder), but a future container-default rollout
    might want the same helper.
  - The regression test stubs `vmaftune.report.probe_source` via
    `monkeypatch.setattr`; the helper is intentionally
    `probe_fn`-injectable to keep test coverage on the helper
    itself independent of the CLI wiring.

## References

- ADR-0505: ladder container-source encode + sample-cloud dedup
  (sibling fix; ADR-0509 closes the compare half of the same bug
  class).
- ADR-0506: V6-1 duration-clamp on encoder side.
- ADR-0508: V8-A pass-1 stats duration-clamp.
- ADR-0501 / ADR-0498: prior BBB e2e clusters establishing the
  container-source / sample-window plumbing pattern.
- Source: `req` (user-provided reproducer + root-cause hypothesis
  in the agent brief 2026-05-18).
