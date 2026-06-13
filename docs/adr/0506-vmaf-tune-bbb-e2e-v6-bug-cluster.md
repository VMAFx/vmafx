<!-- markdownlint-disable MD013 -->
# ADR-0506: vmaf-tune ladder duration clipping, raw-YUV cross-res decode, CLI exit code

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, claude
- **Tags**: vmaf-tune, ladder, corpus, encode, cli, docs

## Context

The BBB end-to-end v6 probe (after PR #1262 / ADR-0505 closed the
v5 cluster) surfaced three follow-ups, all confined to the
`vmaf-tune ladder` orchestration:

- **V6-1** — `ladder --duration N` was metadata-only. The flag is
  used by the sampler to compute kbps (`size_bytes * 8 / N`) but
  never wired into the ffmpeg encode pipe. A 10-second smoke run
  against a 9-minute container source therefore re-encoded the
  full 9 min at every CRF in the sweep — the v6 probe consumed
  ~10 min wall time on a single 3-cell sweep before timing out.
  The reference leg is already clipped by `_maybe_decode_reference`
  (via the `-t` argument added in v2 Bug #v2-A); the encode leg
  never received the same treatment because the encode driver
  only honoured `sample_clip_seconds` (the ADR-0297 sample-clip
  mode), which the ladder CLI does not set.

- **V6-2** — A cross-resolution ladder against a raw-YUV source
  failed on every rung whose target differed from the source dims.
  The v4 reference-side scale path (`_decode_source_to_yuv` with
  `target_width/target_height`) builds an ffmpeg argv of the form
  `ffmpeg -i src.yuv -f rawvideo -pix_fmt yuv420p -vf scale=W:H
  dst.yuv` — no input-side `-f rawvideo -s SRCWxSRCH -r FR` block,
  so ffmpeg's demuxer cannot parse the raw planar bytes and
  refuses the input. The sampler then yields zero scorable
  encodes and the CLI raises
  `RuntimeError: default sampler produced no scorable encodes`.
  The v4 test that pins the scale path used a `.mp4` source, which
  exercises ffmpeg's container auto-detect and silently masks the
  missing raw-input flags.

- **V6-3** — `vmaf-tune ladder` returned exit code 0 even when the
  sampler raised a `RuntimeError` ("default sampler produced no
  scorable encodes"). The Python traceback was printed to stderr
  but the wrapper that calls `main()` returned `0` to the shell,
  defeating CI gates and shell-script error handling. Other
  vmaf-tune subcommands (`compare`, `tune-per-shot`, `report`)
  return `2` on operational failure; `_run_ladder` had no
  try/except around `build_and_emit`.

The V6-1 wall-time bug is the dominant operational pain — every
container-source ladder smoke run currently consumes minutes per
cell instead of seconds. V6-2 defeats the entire purpose of
multi-rung ladders on raw-YUV sources (the single-resolution path
introduced in ADR-0498 works in isolation but cannot be composed
into a ladder). V6-3 is the low-severity CI / scripting bug.

## Decision

1. **V6-1 encode-side duration clamp**: extend `EncodeRequest`
   with a new `duration_s: float = 0.0` field and have
   `build_ffmpeg_command` append `-t duration_s` as an input-side
   flag whenever the caller did NOT opt into sample-clip mode
   (`sample_clip_seconds == 0.0`) AND `duration_s > 0`. Sample-clip
   mode (ADR-0297) keeps precedence because it carries an explicit
   start offset and is centred inside the window — the new flag is
   a "bound the whole encode" clamp, not a per-cell sample window.
   `iter_rows` plumbs `CorpusJob.duration_s` into the new
   `EncodeRequest.duration_s` so the existing `--duration` flag
   exercises the clamp without any CLI changes. The reference
   decode already honours `job.duration_s`; mirroring it on the
   encode side restores the contract the flag's help text already
   promised.

2. **V6-2 raw-YUV demuxer flags in cross-res reference decode**:
   extend `_decode_source_to_yuv` with three new kwargs —
   `source_is_raw`, `source_width`, `source_height` (and
   `source_framerate` for completeness, defaulted to 24 fps when
   unset). When `source_is_raw=True` the argv is rebuilt to insert
   `-f rawvideo -pix_fmt … -s SRCWxSRCH -r FR` BEFORE `-i src.yuv`
   so the demuxer can parse raw planar bytes. The new kwargs are
   `None`/`False` by default so container-source callers (the v4
   pinned path) are unaffected. `_maybe_decode_reference` is
   extended with a matching trio and computes `source_is_raw` from
   the source suffix, then forwards the geometry. `iter_rows`
   passes `job.src_width / src_height` (or, when those are
   `None`, the rung dims as the legacy single-res case) plus
   `job.framerate` into the new kwargs.

3. **V6-3 CLI exit-code on RuntimeError**: wrap the
   `build_and_emit` call in `_run_ladder` in a `try / except
   (RuntimeError, ValueError, OSError)` block that prints the
   exception message to stderr and returns `2`. The exception list
   is intentionally narrow — `KeyboardInterrupt` and unexpected
   `Exception`s still propagate so debug sessions surface the
   traceback. `2` matches the convention used by the sibling
   subcommands.

## Alternatives considered

- **V6-1: route `--duration` through `sample_clip_seconds`.**
  Rejected: sample-clip mode is centred inside the source window
  and carries a start offset. Setting `sample_clip_seconds = N`
  with `duration_s = N` would trip the `requested >= duration`
  guard in `_resolve_sample_clip` and fall back to full-source
  mode anyway, so the path doesn't even compose. The new
  `duration_s` field on `EncodeRequest` is a one-line extension
  that keeps the two concepts orthogonal — "bound the encode to
  N seconds from the start" vs "extract a centred N-second
  window".

- **V6-1: clamp the encode in the ladder CLI by setting
  `CorpusOptions.sample_clip_seconds`.** Rejected for the same
  reason: the `_resolve_sample_clip` precondition rejects
  `requested == duration`. Forcing it to accept would change the
  semantics of `sample_clip_seconds` for every other corpus
  caller.

- **V6-2: probe the source via ffprobe before deciding the
  demuxer flags.** Rejected: the suffix-based detection used
  everywhere else in the corpus pipeline (`_VMAF_RAW_SUFFIXES`)
  is already authoritative — the caller has already declared the
  source's geometry via `CorpusJob.src_width / src_height /
  pix_fmt / framerate`, so ffprobe would only re-derive what we
  already know. The new kwargs are mandatory only when
  `source_is_raw=True`, so the API can't silently accept a missing
  geometry — it raises `ValueError` instead.

- **V6-3: let the exception escape and rely on Python's default
  exit code of 1.** Rejected: the wrapper that invokes
  `_run_ladder` is the CLI dispatcher, which historically
  returns the value `_run_*` produces. Letting the exception
  escape changes the contract for every subcommand and makes
  unit-testing the failure path harder (the test would need to
  assert `pytest.raises(RuntimeError)` instead of `rc != 0`).
  Catching at the subcommand boundary keeps the dispatcher
  surface uniform.

## Consequences

- **Positive**:
  - `ladder --duration 10` against a 9-minute container source
    now consumes ~10 seconds of encode wall time per cell
    instead of 9 minutes. Smoke runs become tractable on long
    sources without pre-cutting via `ffmpeg -ss/-t`.
  - Cross-resolution ladders against raw-YUV sources score
    successfully on every rung; the per-rung reference decode
    now produces a parseable raw YUV file at the rung target.
  - `vmaf-tune ladder` joins the other subcommands in returning
    `2` on operational failure; CI gates and shell scripts can
    rely on the exit byte.

- **Negative**:
  - `EncodeRequest` grows a sixth field that interacts with
    `sample_clip_seconds`; new callers must know which to set.
    The precedence rule is documented inline (sample-clip wins)
    and unit-tested.
  - `_decode_source_to_yuv` now raises `ValueError` when
    `source_is_raw=True` but the geometry is missing. Existing
    callers (V3 / V4 paths) pass `source_is_raw=False` by default
    so no regression; new callers get a loud failure instead of
    a malformed argv.

- **Neutral / follow-ups**:
  - `test_bbb_e2e_v6_bug_cluster.py` pins one regression per
    finding plus a subprocess-driven CLI check for V6-3.
  - `docs/usage/vmaf-tune.md` clarifies that `--duration N` now
    bounds the encode pipe as well as the reference window.
  - The v5 end-to-end docker probe
    (`test_ladder_against_bbb_container_yields_plausible_vmaf`)
    will run much faster post-fix; the 60-second pytest-timeout
    that fired on it pre-fix can stay in place.

## References

- BBB e2e v6 bug log: `/tmp/bbb_e2e_bugs_v6.md` (gitignored)
- Predecessor (v5): [ADR-0505](0505-vmaf-tune-bbb-e2e-v5-bug-cluster.md)
- Predecessor (v4): [ADR-0501](0501-vmaf-tune-bbb-e2e-v4-bug-cluster.md)
- Predecessor (v3): [ADR-0499](0499-vmaf-tune-ladder-reference-decode-v3.md)
- Predecessor (v2): [ADR-0498](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md)
- Encode driver: `tools/vmaf-tune/src/vmaftune/encode.py:build_ffmpeg_command`
- Corpus iter_rows: `tools/vmaf-tune/src/vmaftune/corpus.py:iter_rows`
- Reference decode helper:
  `tools/vmaf-tune/src/vmaftune/corpus.py:_decode_source_to_yuv`
- CLI: `tools/vmaf-tune/src/vmaftune/cli.py:_run_ladder`
- Source: `req` (direct user direction in the agent dispatch
  briefing on 2026-05-18 — paraphrased: "fix the three v6 BBB
  e2e bugs in a single PR: thread `--duration` into the encode
  driver so smoke runs actually clip; fix cross-res raw-YUV
  reference decode by passing source geometry; wrap the CLI
  RuntimeError so the process exits non-zero.")
