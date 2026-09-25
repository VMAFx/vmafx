<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1318: Bound vmaf-perShot scan loop with operator frame ceiling

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `tools`, `cli`, `reliability`, `compatibility`

## Context

[ADR-1287](1287-cli-tool-unbounded-loop-ceilings.md) bounded the `per_shot_scan_loop()`
in `core/tools/vmaf_per_shot.c` using `VMAF_PER_SHOT_MAX_FRAMES` (`UINT32_MAX`),
the width of the `frame_idx` counter stored in shot records. This satisfied NASA/JPL
Power of 10 Rule 2 by establishing a statically provable upper bound on the loop.

However, as documented in ADR-1287's alternatives analysis, `UINT32_MAX` does not
provide a practical operator escape hatch for endless inputs. Scanning `UINT32_MAX`
frames from a stream, named pipe (FIFO), or endless source such as `/dev/zero` requires
reading ~4.29 billion frames (over 1.2 PB at 576x324 YUV420), appearing as an infinite
hang in automated pipelines and workstation use.

ADR-1287 explicitly considered a `--max-frames` option but rejected it on the grounds
that "Changes the default behaviour of an existing tool, and a default low enough to help
is low enough to truncate real content." That rejection assumed an enforced small default.

In addition, ADR-1287 introduced an off-by-one boundary defect: the exhaustion check
`ctx->frame_idx >= VMAF_PER_SHOT_MAX_FRAMES` was evaluated after reading the frame and
after the loop exited. Consequently, an input containing exactly `UINT32_MAX` frames was
rejected with `-EFBIG` even though all frames fit into `uint32_t` without wrapping, limiting
the largest accepted input to `UINT32_MAX - 1` frames.

Exact-head review also exposed a prerequisite for the boundary probe: the reader
loaded luma and then sought over chroma. A regular-file seek beyond EOF succeeds,
so a trailing luma-only or partial-chroma frame was counted as complete. In
addition, `fread() == 0` was treated as EOF without checking `ferror()`, allowing
an I/O fault after earlier frames to truncate the plan successfully. A safe
ceiling must count complete raw frames and distinguish true EOF from read failure.

Ticket `T-PER-SHOT-ENDLESS-INPUT-NOT-A-TIMEOUT-2026-09-21` calls for resolving the endless-input
hang with the smallest backward-compatible operator-visible bound while preserving ordinary
finite inputs and eliminating the `uint32_t` off-by-one boundary error.

## Decision

1. **Operator Frame Ceiling (`-F, --frames <N>`)**:
   Add an explicit operator option `-F, --frames <N>` to `vmaf-perShot`, along with aliases
   `--frame_cnt <N>` and `--max-frames <N>`.
2. **Unbounded Compatibility Contract**:
   The default value of `max_frames` is `0U` (unbounded / all frames). Finite files and existing
   scripts run without behavioural change, scanning until EOF.
3. **Clean Early Termination**:
   When `s->max_frames > 0U`, the scan loop stops after reading and processing `s->max_frames`
   frames, exiting cleanly with code 0 and outputting the computed per-shot plan for the
   bounded prefix.
4. **Off-by-One Fix and Safe 64-bit Indexing**:
   `ctx->frame_idx` in `struct per_shot_scan_ctx` is widened to `uint64_t`. When `s->max_frames == 0U`,
   the loop ceiling is `(uint64_t)VMAF_PER_SHOT_MAX_FRAMES + 1ULL`. At frame index `VMAF_PER_SHOT_MAX_FRAMES`,
   the reader attempts to read a frame:
   - If EOF is reached, the loop exits cleanly and returns 0, correctly accepting an input of
     exactly `UINT32_MAX` frames.
   - If a frame is read beyond `VMAF_PER_SHOT_MAX_FRAMES`, `vmaf-perShot` emits
     `input exceeds the 4294967295-frame scan limit` to stderr and returns `-EFBIG`.
5. **Complete-frame, fail-closed probe**:
   Move raw input consumption into a private reader module. It reads luma and
   chroma exactly, treats EOF as clean only before the first byte of a new luma
   plane, and returns distinct results for a partial frame and `ferror()`. The
   scan emits a diagnostic and fails with `-EIO` for either corrupt input or an
   underlying read error. Chroma is consumed rather than sought over because a
   successful seek does not prove that those bytes exist.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Explicit `--frames` flag defaulting to 0 (unbounded)** (chosen) | Provides a clean operator escape hatch for FIFOs and sampling; 100% backward compatible; deterministic; preserves finite input behaviour | Requires user or caller script to specify the flag when feeding endless sources | — |
| Wall-clock timeout on scan loop | Automatically aborts endless streams without user intervention | Makes scan success depend on system load, disk I/O, and CPU contention; turns long legitimate scans into flaky failures | Rejected by ADR-1287 and reaffirming rejection here |
| Small default frame ceiling (e.g. 100 000 frames) | Prevents runaway execution by default | Silently truncates legitimate long-form video files and multi-hour content; breaks backward compatibility | Rejected by ADR-1287 |
| Reject non-regular files (`stat` check for S_ISFIFO / S_ISCHR) | Prevents opening FIFOs and `/dev/zero` | Breaks legitimate video piping workflows (e.g., streaming from ffmpeg or decoder via pipe) | Unnecessarily restricts valid UNIX pipeline composition |
| Seek over chroma after checking regular-file length | Avoids copying chroma on immutable regular files | Does not cover pipes; duplicates platform-specific size/offset handling; mutable files introduce a check/use race | Exact consumption gives one fail-closed contract for files and streams; optimise only after measurement in the later tuning phase |

## Consequences

- **Positive**:
  - Operators and automated test harnesses can bound scans on FIFOs, `/dev/zero`, or test fixtures to a fixed frame count.
  - The off-by-one error is fixed: valid inputs of exactly `UINT32_MAX` frames are accepted without premature `-EFBIG` failure.
  - Partial raw frames and read errors can no longer become successful prefix plans.
  - Full backward compatibility is preserved: default invocation scans all available frames.
- **Negative**:
  - Unbounded reads on endless inputs still run indefinitely if the operator omits `--frames`. This is documented as the expected UNIX streaming contract.
  - The luma-only algorithm now reads and discards chroma instead of seeking over it. Performance work is intentionally deferred to the post-correctness tuning phase.
- **Neutral / follow-ups**:
  - Updates CLI documentation, `core/tools/AGENTS.md`, and closes `T-PER-SHOT-ENDLESS-INPUT-NOT-A-TIMEOUT-2026-09-21` in `docs/state.md`.

## References

- [ADR-1287](1287-cli-tool-unbounded-loop-ceilings.md) — initial unbounded loop ceilings in fork CLI tools.
- [ADR-1142](1142-whole-codebase-standards.md) — whole-codebase standards and NASA/JPL Power of 10 rules.
- Ticket: `T-PER-SHOT-ENDLESS-INPUT-NOT-A-TIMEOUT-2026-09-21`.
- Source: req — Lusoris operator ticket.
