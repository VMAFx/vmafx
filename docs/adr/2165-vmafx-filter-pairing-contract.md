<!-- markdownlint-disable MD013 MD060 -->
# ADR-2165: The `vmafx` filter scores only frames it can pair, and reports what it cannot

- **Status**: Proposed
- **Date**: 2026-10-07
- **Deciders**: maintainer (FFmpeg audit request, RC4 WP15); RC4 WP15
- **Tags**: rc4, ffmpeg, correctness

## Context

The `vmafx` filter ([ADR-2125](2125-vmafx-ffmpeg-gstreamer-filters.md)) pairs a
main frame with a reference frame through FFmpeg's framesync. The audit of
everything VMAFx touches in FFmpeg ([Research-2166](../research/2166-ffmpeg-audit-2026-10-07.md))
ran the filter on the Netflix 576x324 pair and found four ways to get a
number that does not mean frame against its own reference frame, or no number
and a failing command, without a word from the filter:

1. The framesync default `eof_action=repeat` repeats the last frame of a
   reference that ends early. A reference of 24 frames under a main of 48
   scored 89.659227; the CLI over the 24 paired frames, and the filter with
   `eof_action=endall`, give 96.580028.
2. Streams of different frame rate are paired by timestamp: 25 fps against
   30 fps scored 81.745866 where the same frames at one rate score 95.938231.
3. A run in which no pair was scored (`enable=` disabled for every frame, or
   an empty stream) called `vmafx_flush()` on an empty context, which refuses
   (`-22`), and ffmpeg exited 187 although every frame had passed.
4. A `log_path` in a missing directory failed only at uninit, where a failure
   can no longer fail the command: exit 0, no report.

Upstream's `libvmaf` filter has the same defaults. The old filters logged the
input order at init; `vmafx` logged nothing.

## Decision

1. **The framesync defaults of the scoring filters are `eof_action=pass` and
   `repeatlast=0`** (`vmafx` and `vmafx_tune`). A main frame with no reference
   frame passes on unscored, in order (also behind frames `metadata=1` holds),
   and is counted: one warning at the first, and
   `N main frames had no reference frame and passed unscored; the scores cover M
   pairs` at the end. `eof_action=repeat:repeatlast=1` restores the old
   behaviour; `repeat` alone has no effect because FFmpeg's framesync turns
   `repeatlast=0` into `pass`.
2. **Pairs across a timestamp offset are reported, not refused.** A configure
   warning when the two inputs' frame rates differ, and a count of pairs whose
   timestamps differ by more than half a main frame, with a warning at the
   first and a total at the end. Refusing would break legitimate offsets
   (decoder delay, edit lists); the warning names what to align.
3. **No pair scored is not an error.** The filter does not flush a context
   that was never given a frame, writes no report (and warns that `log_path`
   was not written), and logs no score.
4. **A requested output that cannot be produced fails at init.** `log_path` is
   opened for append at configuration; failure fails the graph.
5. **The input order is logged once** (`input 0 (main) is the distorted video,
   input 1 (reference) the reference`), as the `libvmaf` filter did.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Default `eof_action=endall` | Ends both streams, so no unscored frame exists | In an encode-and-score graph the main stream is also the encoder's input: the encode would stop at the shorter input and lose frames silently | A different silent failure for the same graph |
| Fail the graph when a main frame has no reference | Strictest | Breaks the pass-through use (score part of a stream); `eof_action=pass` is FFmpeg's own spelling of "the main stream continues" | Counting and warning keeps the stream and the evidence |
| Keep the framesync defaults, document them | No behaviour change | The wrong number stays the default; an encode-and-score user never reads the framesync options | The defect is the default |
| Refuse timestamp offsets above half a frame | A wrong pairing cannot score | Decoder delay and edit lists give legitimate offsets of whole frames; the filter cannot tell them from a wrong cut | Warn and count; the user decides |
| Probe `log_path` at the end only (status quo) | None | Cannot fail the command | The failure is invisible to scripts |

## Consequences

- **Positive**: a score printed by `vmafx` is a score of frames that were
  paired, or the log says how many were not; an empty run succeeds; a bad
  report path stops the run before the work.
- **Negative**: the defaults differ from the stock `libvmaf` filter for users
  who relied on `repeat`; the migration guide says how to restore it.
  `log_path` is created (empty) at init when it does not exist.
- **Neutral / follow-ups**: `ffmpeg-patches/test/vmafx_filter_check.py contract`
  (seven checks, six failing on the previous head, the matched pair passing
  both) guards it; the GStreamer element has its own pairing (`GstAggregator`)
  and is not changed here.

## References

- RC4 WP15 brief (FFmpeg audit, 2026-10-07): the maintainer asked for an audit of
  everything VMAFx touches in FFmpeg, correctness of the filters' paths
  included, and for the correctness findings to be fixed (paraphrased).
- [Research-2166](../research/2166-ffmpeg-audit-2026-10-07.md): findings C-1 to C-5.
- `ffmpeg-patches/src/vf_vmafx.c`, `docs/usage/ffmpeg.md` (Pairing the two
  inputs).
