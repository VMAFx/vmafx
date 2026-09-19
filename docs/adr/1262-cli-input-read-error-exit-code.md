<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1262: A failed input read exits 102; a legitimately shorter stream stays exit 0

- **Status**: Proposed
- **Date**: 2026-09-19
- **Deciders**: Lusoris
- **Tags**: cli, tools, exit-codes, fork-local

## Context

`run_frame_loop()` in `core/tools/vmaf.cpp` returned a single `unsigned`: the number of
frames it consumed. Every reason the loop could stop — both streams ending, one stream
ending early, a reader returning an error, `vmaf_read_pictures()` failing — collapsed into
that one number, so `main()` had no way to tell a completed run from a failed one. The
result was that **`vmaf` exited 0 on every input read failure** and wrote a full report over
whatever prefix had arrived.

A second defect compounded it. `fetch_picture()` returns `1` at end of stream and `-1` on a
read error, and the branch chain tested `ret1 && ret2` *before* `ret1 < 0 || ret2 < 0`. Both
`-1` values satisfy the first test, so when **both** inputs failed the loop classified it as a
clean end of stream: no diagnostic at all, and the same exit 0. Measured on `master`
(`ef1c16071`) with a pair of y4m clips truncated mid-frame: exit 0 and a 697-byte JSON
report. Upstream Netflix/vmaf carries the same ordering; it was noted there as known and
left unfixed in [Netflix/vmaf#1604](https://github.com/Netflix/vmaf/pull/1604).

This matters because the exit status is the whole interface for automation. A CI gate, a
`vmaf-tune` bisect predicate, or any shell wrapper that tests `$?` could not distinguish a
corrupt input from a clean short one, and `docs/usage/cli.md` already promised that code 1
meant "any parse / I/O / runtime error" — a promise the binary did not keep.

## Decision

`run_frame_loop()` returns a `FrameLoopResult { frames, exit_code }`, and `main()` fails the
run when `exit_code` is non-zero, ahead of the existing no-frames guard. A read failure exits
with the new dedicated code **`VMAF_EXIT_INPUT_READ_ERROR` (102)** and writes no report.
The error test is reordered ahead of the end-of-stream test so two failed reads are
classified as the error they are.

A stream that merely **ends earlier than its partner** is deliberately *not* an error: it keeps
its `"…" ended before "…"` warning, its report, and exit 0. Scoring the common prefix of a
legitimately shorter file is a supported use, and silently turning it into a failure would
break callers that rely on it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Reuse the generic `1` / `-1` failure path | No new code to document; matches the existing "non-zero-but-unspecified" convention | A caller cannot separate "the file would not read" from a bad flag, a missing model or a failed output write — exactly the discrimination `vmaf-tune`'s bisect predicate needs | The repository already chose dedicated codes for this class of ambiguity (ADR-0543 → 100, no-frames → 101); a third one is the consistent move, not a new pattern |
| Reuse `VMAF_EXIT_NO_FRAMES_DECODED` (101) | No new constant at all | Conflates "the inputs were empty or too short" with "bytes were expected and the read failed" — the second leaves a truncated prefix that may already have been scored, the first leaves nothing | The two want different operator responses: re-check the `--frame_skip_*` arithmetic versus re-fetch the media |
| Also fail on a length mismatch (`"…" ended before "…"`) | One uniform rule: any short read fails | Breaks every caller that intentionally scores a shorter distorted clip against a longer reference, which the warning exists to support | Out of scope for a correctness fix, and a behaviour change with real users behind it; left as a separate decision if it is ever wanted |
| Keep the count-only return and have `main()` re-derive failure from the stream state | No signature change | The reader's error is already gone by the time `main()` runs; re-deriving it means re-reading or duplicating reader state | Reconstructs information the loop already had and threw away |

## Consequences

- **Positive**: a corrupt or truncated input is now detectable by exit status alone. No report
  file is written for a run that failed to read its input, so a stale or partial JSON can no
  longer be mistaken for a fresh result. `docs/usage/cli.md`'s exit-code table becomes true.
- **Negative**: a caller that today tolerates truncated media and consumes the partial score
  will start seeing a non-zero exit. That is the point of the change, but it is a
  behaviour change and is called out in the changelog as such.
- **Neutral / follow-ups**: `core/tools/test/test_vmaf_read_error_exit.sh` pins all four
  cases in the `fast` suite. Whether a length mismatch should also fail is left open above.
  The three Netflix golden pairs are byte-identical before and after — this touches only the
  status a run reports, never a score.

## References

- `core/tools/vmaf.cpp` — `classify_frame_fetch()`, `FrameLoopResult`, `run_frame_loop()`.
- `core/tools/test/test_vmaf_read_error_exit.sh` — the four-case regression test.
- [ADR-0543](0543-adr-0498-enforcement-hardening.md) — the dedicated-exit-code precedent (100).
- [Netflix/vmaf#1604](https://github.com/Netflix/vmaf/pull/1604) — upstream fixes the related
  `!ret` mapping (already correct in this fork via `finish_unread_picture()`) and records the
  `ret1 && ret2` ordering as knowingly left unfixed.
- Source: `req` — the user's standing direction to fix every defect found rather than defer
  it ("pre-existing is no excuse ever").
