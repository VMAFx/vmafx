**`vmaf-tune tune-per-shot`: expose `--scene-threshold` + `--max-shot-duration`; render 1-shot timeline (ADR-0513)**

The `vmaf-perShot` luma-delta heuristic (compiled-in cutoff `12.0` on
8-bit content) under-cuts short clips — a 5 s BBB 4K segment returned a
single shot for `[0, 300)`, defeating per-shot tuning. The
`tune-per-shot` Python wrapper now exposes:

- `--scene-threshold X` — forwards to the C binary as
  `--diff-threshold X`. Lower values yield more shots. Default
  unset (preserves the C-side default).
- `--max-shot-duration S` — uniform-time-window splitter
  (default `2.0` seconds, `0` disables). Any detected shot
  longer than `S` is sliced into equal-length sub-shots so the
  per-shot tuner always sees a multi-shot timeline on clips of
  duration `> S`.

The HTML/Markdown report's per-shot timeline chart now renders an
explicit `ax.hlines(...)` band over each shot's frame range plus
midpoint markers. Previously the `ax.step([start], [crf], ...)` call
on a 1-shot dataset produced a zero-length path the SVG backend
silently dropped — the chart canvas was blank even though axes,
legend, and title rendered. Observed in
`.workingdir/bbb_reports/bbb_2160p60_v9_PROPER_*.html`.

+6 regression tests across `tools/vmaf-tune/tests/test_per_shot.py`
(splitter contract + threshold forwarding + 5 s @ 60 fps splits to
`>= 2` shots) and `tools/vmaf-tune/tests/test_report.py` (1-shot SVG
chart contains a non-empty `<path>` / `<line>` element).
