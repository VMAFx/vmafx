<!-- markdownlint-disable MD013 MD060 -->
# ADR-0532: tune-per-shot tolerates read-only CWD when writing segments

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vmaf-tune`, `cli`, `robustness`, `container`

## Context

`vmaf-tune tune-per-shot` writes its encoding plan as a JSON file
(`--plan-out`) and also writes a concat-demuxer listing
(`<seg-dir>/concat.txt`) for the FFmpeg concat step.  The plan JSON is
the primary deliverable — downstream scripts consume it to schedule the
per-shot encodes.  The concat listing is a convenience artifact whose
absence does not block plan consumption.

The bug was discovered when running the BBB 4K v10 report regeneration
inside the `vmaf-dev-mcp` container: the `/workspace` bind-mount is
read-only from the host.  The default segment directory resolved to
`Path("per_shot_encode.mp4").parent / "segments"` — i.e.
`Path("segments")` relative to CWD (`/workspace`).  The `mkdir` call in
`write_concat_listing` raised `PermissionError`, causing the command to
exit 1 even though the plan JSON had been fully written.  Downstream
scripts treat any non-zero exit as a failure and abort the pipeline.

Two independent root causes combined to produce the failure:

1. **Segment-dir derivation used `args.output.parent`** — when `--output`
   defaults to the relative path `per_shot_encode.mp4`, the parent is
   `.` (CWD), not the directory containing the plan JSON.
2. **`write_concat_listing` errors were not caught** — the `OSError`
   propagated uncaught and terminated the command with a non-zero exit.

## Decision

Apply two complementary fixes, each independently sufficient to handle
the common container case:

1. **Priority order for segment-dir derivation** (in `cmd_tune_per_shot`):
   - Use `--segment-dir` when explicitly set (unchanged).
   - Otherwise use `--plan-out`'s parent directory (new: `plan_out.parent / "segments"`).
     The plan JSON was just written there, so the parent is guaranteed writable.
   - Fall back to `args.output.parent / "segments"` only when neither flag is set.

2. **Graceful fallback** (in `cmd_tune_per_shot`):
   Wrap the `write_concat_listing` call in a `try/except OSError`.
   On failure emit `WARN: segments dir <path> not writable; skipping
   concat listing (<exc>). Plan JSON still emitted at <plan_out>.` to
   stderr and continue — exit 0.  The plan JSON is the primary deliverable
   and is always fully written before this call.

The `write_concat_listing` function itself is unchanged; the fallback
lives entirely in the CLI layer where the I/O policy belongs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Only add the OSError catch (fix 2 alone) | Minimal diff | Segments still attempt to land in CWD; WARN fires on every container run even when `--plan-out` is set to a writable path | Fix 1 alone already prevents the error in the common case; fix 2 is a belt-and-suspenders backstop for cases where even `plan_out.parent` is non-writable |
| Only fix the seg-dir derivation (fix 1 alone) | Prevents error in common container case | Non-writable `plan_out.parent` or non-writable explicit `--segment-dir` still exits 1 | Belt-and-suspenders catch needed for edge cases |
| Add `--segment-dir` as a required flag when plan JSON is requested | Explicit, no ambiguity | Breaking change to the CLI; forces all existing scripts to update | CLAUDE.md §12 r10 prohibits breaking user-discoverable surfaces without a Supersedes-ADR and migration path |
| Move the fallback logic into `write_concat_listing` | Single call site | `write_concat_listing` is a pure I/O helper; policy (warn + continue) belongs in the CLI | Breaks the separation of concerns; makes the helper untestable in isolation |

## Consequences

- **Positive**: `tune-per-shot --plan-out /tmp/plan.json` exits 0 inside
  a container whose `/workspace` bind-mount is read-only.  The plan JSON
  is always the authoritative deliverable; the concat listing is written
  alongside it in the common case.
- **Positive**: Belt-and-suspenders catch means the command is robust
  even if `plan_out.parent` itself is non-writable (e.g. a read-only
  NFS mount for `--plan-out`).
- **Neutral**: `--segment-dir` default documentation in
  `docs/usage/vmaf-tune.md` updated to reflect the three-tier priority
  order.
- **Negative**: None.  The concat listing location changes from
  `<output>.parent/segments/concat.txt` to `<plan_out>.parent/segments/concat.txt`
  when `--plan-out` is set without `--segment-dir`.  In the common case
  both `--output` and `--plan-out` are in the same directory, so the
  location is identical.

## References

- Discovered during BBB 4K v10 report regeneration in `vmaf-dev-mcp` container (user direction).
- `tools/vmaf-tune/src/vmaftune/cli.py` — `cmd_tune_per_shot` (fixed).
- `tools/vmaf-tune/tests/test_per_shot.py` — two new regression tests.
- `docs/usage/vmaf-tune.md` — `--segment-dir` default documentation updated.
- Related: [ADR-0392](0392-vmaf-tune-phase-d-per-shot.md) (Phase D per-shot design),
  [ADR-0513](0513-per-shot-scene-threshold-and-1-shot-chart.md) (scene-threshold + uniform-window splitter).
