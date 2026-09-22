<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1287: Bound the fork CLI tools' open-ended read and retry loops

- **Status**: Accepted
- **Date**: 2026-09-21
- **Deciders**: Lusoris
- **Tags**: `tools`, `cli`, `reliability`, `docs`

## Context

Two fork-local CLI tools drove a `for (;;)` whose only exits were "got a
frame", "end of stream" and "hard error":

- `per_shot_scan_loop()` in `core/tools/vmaf_per_shot.c` reads raw YUV frames
  until `per_shot_read_luma()` reports zero bytes.
- `vpl_decode_frame()` in `core/tools/vmaf_vpl.c` calls
  `MFXVideoDECODE_DecodeFrameAsync()` and, on `MFX_ERR_MORE_SURFACE` or
  `MFX_WRN_DEVICE_BUSY`, sleeps 1 ms and asks again.

Neither loop has a termination argument that survives a hostile or degraded
input. A reference path that never reaches end of file — a FIFO held open by a
writer, a character device such as `/dev/zero`, a growing file — keeps the
per-shot scan running with no diagnostic, and `per_shot_record_frame()` stores
the frame index in a `uint32_t`, so past `UINT32_MAX` the numbering would wrap
and silently corrupt the shot table. A VPL device that keeps reporting
`MFX_WRN_DEVICE_BUSY` keeps `vpl_decode_frame()` spinning at 1 ms per attempt
for as long as the process lives. `vpl/mfxvideo.h` (oneVPL 2.17, the version
this host builds against) documents the recovery as "Call this function again
after `MFXVideoCORE_SyncOperation` or in a few milliseconds" and specifies no
ceiling, so any ceiling is a fork policy choice rather than an API contract.

NASA/JPL Power of 10 rule 2 (see [principles.md §1.1](../principles.md#11-nasajpl-power-of-10-rules-adapted-for-c)) requires
a statically provable upper bound on every loop; ADR-1142 extends the fork's
standards to the whole tree, `core/tools/` included.

## Decision

Each loop gets an explicit ceiling chosen from a quantity the surrounding code
already depends on, and reports its exhaustion instead of continuing:

- `VMAF_PER_SHOT_MAX_FRAMES` is `UINT32_MAX`, the width of the frame counter
  the shot records store, used as the loop's *exclusive* bound. Exhaustion
  prints `vmaf-perShot: input exceeds the <N>-frame scan limit` and returns
  `-EFBIG`. The test sits after the loop, so the accepted maximum is
  `UINT32_MAX - 1` frames — one short of the counter's range, and about a
  petabyte of input at 576x324, so the gap is documented rather than closed.
- `VPL_DECODE_MAX_ATTEMPTS` is `VPL_SYNC_TIMEOUT_MS * 1000 / VPL_DECODE_RETRY_US`
  = 60 000 attempts, derived from the 60 s ceiling the same function already
  hands `MFXVideoCORE_SyncOperation()` and the 1 ms back-off it already used.
  Exhaustion prints `DecodeFrameAsync yielded no frame after <N> attempts` and
  returns `-1`, which the run loop reports as a decode error.

Both ceilings are documented in the tools' user pages, including what they do
*not* guarantee.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Per-shot: bound at `UINT32_MAX`** (chosen) | The bound is the correctness limit that already exists in the data structure, not an invented number; needs no new CLI surface | Not a practical hang timeout — reaching it still means ~4.29e9 frame reads, so an endless FIFO still *looks* like a hang. The ceiling is also tested after the loop rather than before the next read, so it is conservative by exactly one frame: an input of `UINT32_MAX` frames is rejected although all of them were numbered without wrapping, and the largest accepted input is `UINT32_MAX - 1` frames | — |
| Per-shot: wall-clock timeout on the scan | Actually cures the perceived hang | Makes a correct run's success depend on machine speed and I/O contention; a legitimately slow 4-hour scan on a loaded box would start failing | Turns a deterministic tool into a flaky one |
| Per-shot: new `--max-frames` flag defaulting to something small | Gives the operator a real escape hatch | Changes the default behaviour of an existing tool, and a default low enough to help is low enough to truncate real content | Behaviour change on an existing surface; the ceiling would be arbitrary |
| **VPL: attempt counter sized to the existing 60 s sync ceiling** (chosen) | Reuses a timeout the tool already imposes on the same device in the same function, so no new number is invented | Counts non-sleeping `MFX_ERR_MORE_DATA` refill iterations against the same budget, so the ceiling is "60 000 attempts", not exactly "60 s" | — |
| VPL: count only the sleeping retries | The bound would be an exact 60 s wall clock | Leaves the `MFX_ERR_MORE_DATA` path unbounded again — a decoder that consumes input and never emits a frame would still spin | Reintroduces the defect in a narrower form |
| VPL: `clock_gettime()` deadline instead of a counter | Exact wall clock, robust to mixed fast/slow iterations | A clock read per attempt, and the bound stops being statically provable (Power of 10 rule 2) | Loses the static termination argument the change exists to establish |
| Leave both loops unbounded | No new failure modes | Leaves a silent hang and a `uint32_t` wrap in shipped tools | Rejected |

## Consequences

- **Positive**: both loops terminate by construction; the per-shot frame
  numbering can no longer wrap; a wedged VPL device produces a diagnostic
  instead of a process that has to be killed. Restructuring
  `vpl_host_upload_fallback()` around `VplFallbackState` also retires the
  `clang-analyzer-deadcode.DeadStores` `NOLINT` that used to sit on
  `have_dis_pic`: the store lives in `vpl_fallback_alloc_pictures()` and every
  read lives in `vpl_fallback_release()`, so an intra-procedural dead-store
  analysis no longer sees a dead store. Measured caveat: with clang-tidy
  22.1.8 and this repository's `.clang-tidy` the finding does not reproduce on
  the parent file either once the `NOLINT` line is deleted, so the suppression
  was already inert at this toolchain version — removing it is correct, but it
  is not evidence that a live finding was fixed.
- **Negative**: two new user-visible failure modes. `vmaf-perShot` can now
  exit with `EFBIG`, and `vmaf_vpl` can now abandon a decode that the old
  `for (;;)` would have kept retrying. The VPL ceiling has **not** been
  exercised against real Intel hardware in this change — it is reasoned from
  the tool's own existing 60 s sync timeout, not measured. Both are tracked in
  [state.md](../state.md).
- **Neutral / follow-ups**: `core/tools/vmaf_vpl.c` drops from 21 to 12
  clang-tidy findings in the `sycl` lane, so `scripts/ci/tidy-baseline-sycl.json`
  is stale-high. No gate reads it today — CI only ratchets the `cpu` lane, and
  the `Tidy SYCL` job is advisory — but ADR-1142 still asks a cleaned file to
  tighten its baseline in the same change, so it wants
  `make tidy-ratchet-write LANE=sycl` on a SYCL-capable build. Tracked as
  `T-TIDY-BASELINE-SYCL-STALE-VPL-2026-09-21` in [state.md](../state.md).

## References

- `vpl/mfxvideo.h`, oneVPL 2.17 — `MFX_WRN_DEVICE_BUSY`: "Call this function
  again after `MFXVideoCORE_SyncOperation` or in a few milliseconds."
- [ADR-1142](1142-whole-codebase-standards.md) — whole-tree standards and the
  clang-tidy ratchet.
- [ADR-0977](0977-core-tools-input-reader-safety.md) — the `size_t`-precision cast
  that moved into `yuv_input_set_plane_geometry()` with the arithmetic it
  protects.
- [ADR-0141](0141-touched-file-cleanup-rule.md),
  [ADR-0278](0278-t7-5-nolint-sweep.md) — the touched-file cleanup rule
  and the NOLINT this change discharges.
- Branch `chore/hiss21-core-tools`, commits `437c849e4`, `e04f1dbe8`.
- Source: `req` — the maintainer's standing direction that the fork's own
  gates are fixed at the root rather than suppressed.
