<!-- markdownlint-disable MD013 MD060 -->
# ADR-0513: Expose `--scene-threshold` + `--max-shot-duration`; render 1-shot timeline chart

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: vmaf-tune, per-shot, report, ux

## Context

Two related defects surfaced from a 5 second 3840x2160 Big Buck Bunny
encode targeting `vmaf-tune tune-per-shot`:

**Bug A — scene detector under-cuts short clips.** `vmaf-perShot` uses
a mean-absolute-luma-delta heuristic with a compiled-in default cutoff
of 12.0 (8-bit domain). On a 5 s BBB 4K segment the heuristic returned
a single shot covering frames `[0, 300)` even though the source
visibly contains multiple cuts. The Python wrapper did not expose a
way to lower the cutoff, and the harness had no fallback for the
degenerate case where the detector misses every cut. The downstream
per-shot tuner therefore degraded to a single CRF/VMAF pair for the
whole clip, defeating the purpose of per-shot tuning.

**Bug B — 1-shot timeline chart is empty.** The HTML report renderer
called `ax.step([start_frame], [crf], where="post")` on a single-shot
dataset. Matplotlib emits a zero-length path in that case, which the
SVG backend renders as a `d="M x y"` with no `L` segment — the axes,
title, and legend draw correctly but no line, point, or band appears
on the canvas. The v9 BBB 4K profile-card report exhibited this on
`.workingdir/bbb_reports/bbb_2160p60_v9_PROPER_20260518_1031.html`.

Both defects share a root cause: the per-shot pipeline assumes
multi-shot output and degrades silently when only one shot is
produced. The fix targets both ends — give operators a way to extract
more shots *and* make the report honest when the detector still
returns one.

## Decision

We will:

1. Add `--scene-threshold FLOAT` to `vmaf-tune tune-per-shot` that
   forwards as `--diff-threshold` to the `vmaf-perShot` C binary.
   Lower values yield more cuts. Default `None` (keep C-side default
   12.0) so existing pipelines see no behavioural change unless the
   operator opts in.
2. Add `--max-shot-duration FLOAT` (seconds; default `2.0`,
   `0` disables) that applies a uniform-time-window splitter on top
   of the detector output. Any shot longer than the window is sliced
   into equal-length sub-shots (within ±1 frame of each other). This
   guarantees that a 5 s clip always produces ≥ 2 shots — the
   detector can never under-cut below `ceil(duration / window)`.
3. Replace the `ax.step(starts, crfs, where="post")` calls in
   `_shot_plot_fn` with explicit `ax.hlines(...)` bands over each
   shot's `[start_frame, end_frame)` range plus midpoint markers.
   This renders correctly for any shot count `≥ 1` and gives the
   1-shot case a visible full-width band labelled with the tuner's
   pick. Explicit `set_xlim` / `set_ylim` bounds guard against
   matplotlib's autoscale degeneracy on single-shot inputs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Lower the C-side default `VMAF_PER_SHOT_DEFAULT_DIFF_THRESHOLD` from 12.0 to e.g. 6.0 | One-line C fix; benefits every caller | Changes default behaviour for every existing caller including the BBB e2e fixtures; would shift per-shot snapshot counts and require regenerating all `testdata/scores_cpu_*.json` snapshots | Preserves existing snapshots; operators get the override when needed |
| Replace luma-delta heuristic with a real TransNet V2 inference path | Detector quality matches research; ADR-0223 already wires the model | Requires GPU runtime + ONNX dispatch on the hot path; per-shot turnaround stops being a 50 ms operation | Out of scope for a UX bug fix; revisit when ADR-0223 lands the runtime |
| Make `--max-shot-duration` disabled by default | Keeps the wrapper a transparent passthrough | Re-introduces the silent 1-shot degeneracy that triggered this ADR; the default `2.0 s` window is the minimum viable behavioural change | Default-on is the safer ergonomic choice for the per-shot tuner |
| Render the 1-shot chart as a single scatter point instead of an hline band | One-line `ax.scatter` replacement | Doesn't visually communicate the *extent* of the shot — user can't tell from the chart that the shot spans the whole clip | hline band conveys both the value and the frame range |
| Skip the chart entirely when `len(shots) == 1` | No drawing-degeneracy possible | Removes useful information; operators lose the visual "the tuner saw one shot" signal | The band rendering is strictly more informative |

## Consequences

- **Positive**: `tune-per-shot` produces a multi-shot timeline on
  short clips by default; reports always render a drawable chart
  element regardless of shot count; operators can dial scene
  sensitivity from the CLI without rebuilding the C binary.
- **Negative**: existing callers that relied on the silent
  single-shot fallback will now see uniform-window slicing — the
  resulting CRFs may differ from the prior single-shot pick.
  Operators can restore the old behaviour with
  `--max-shot-duration 0`.
- **Neutral / follow-ups**: docs/usage/vmaf-tune.md documents both
  flags; tests/test_per_shot.py covers the splitter and the
  threshold-forwarding path; tests/test_report.py asserts the 1-shot
  chart contains a non-empty drawable element. The follow-up TransNet
  V2 dispatch is tracked separately under ADR-0223.

## References

- `req`: paraphrased — "vmaf-tune tune-per-shot returns 1 shot on
  5 s BBB 4K and the per-shot timeline chart is empty when there's
  only 1 shot; expose a scene-threshold flag and render a visible
  chart element for the 1-shot case."
- Related: ADR-0222 (vmaf-perShot binary), ADR-0223 (TransNet V2
  port), ADR-0506 (BBB e2e v6 bug cluster), ADR-0511 (MCP backend
  probe + ladder backend wiring).
- PR: TBD.
