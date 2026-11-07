# Idle-window T0 calibration (issue #3294)

The per-run wall-clock reports behind the 2026-11-07 idle-window rerun on
issue #3294, committed at the maintainer's request as the calibration data
for the benchmark baseline ("please do commit the 24 per-run JSON reports —
they are the calibration data for the benchmark baseline").

- 24 runs: `{cpu, cuda}` x N in {10, 30, 60, 120} x 4 runs per cell; run 0
  is the cold run, discarded from the minimum per the maintainer's
  2026-11-04 guidance; the minimum is taken over runs 1-3.
- `summary.json` — per-cell min-of-3 walls, the matched run's `fps` and
  `wall_fps`, per-cell nvidia-smi utilization gates, and the OLS fits
  (primary over N >= 30, secondary over all four points).
- `REPORT.md` — the environment statement, the min-of-3 table, both fits,
  and the load-throttling discriminator verdict.
- `measure.py` — the generator: rc.3 `vmafx.exe`, 1920x1080 yuv420p8 from
  the t1_frames fixture pair, full-process wall via `time.perf_counter`,
  every run's JSON report kept.

Headline (RTX 5080, `CUDA_VISIBLE_DEVICES=1`): CPU T0 = 31.1 ms at
21.305 ms/frame (R^2 = 1.0000); CUDA T0 = 403.1 ms at 6.300 ms/frame
(R^2 = 0.9788), crossing near N = 21. On the quiet GPU the scoring window
runs at 127-153 fps for CUDA against ~47 for CPU — the loaded-run parity
(33.4 vs 33.1 fps) was external-load throttling, not a shared in-loop cost.
The machine is quieter, not idle: utilization gates read ~22-26 % with
occasional 44-58 % compositor spikes; even the worst timed CUDA run
(92.7 fps, taken during a spike) stays ~2x CPU.
