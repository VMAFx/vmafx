# #3294 idle-window full matrix — RTX 5080, 2026-11-07

## Environment

- Machine: Windows 10.0.26300 x64. Two NVIDIA GPUs: device 0 = NVIDIA GeForce
  RTX 4060 Laptop GPU, device 1 = **NVIDIA GeForce RTX 5080** (the device under
  test; CUDA runs pinned via `CUDA_VISIBLE_DEVICES=1`).
- Exe provenance: `C:\PLAN\VMAF\verify\vmafx-install\bin\vmafx.exe` — **rc.3**
  (static default model), 291,737 bytes, dated Nov 3 15:21.
- Input: `C:\PLAN\VMAF\baseline\t1_frames\ref_1080p8_{N}.yuv` /
  `dis_1080p8_{N}.yuv` — 1920x1080 yuv420p 8-bit raw, N in {10, 30, 60, 120}.
- Method: {cpu, cuda} x N, 4 runs per cell; run 0 cold discarded, min over
  runs 1-3; full-process wall time via `time.perf_counter`; per-run JSON kept
  in this directory. Harness: `C:\PLAN\VMAF\verify\tmp\t0_idle_matrix.py`,
  console log `C:\PLAN\VMAF\verify\tmp\t0_idle_matrix_run.log`.

### Utilization situation (nvidia-smi `-i 1`, 1 s apart, 4 samples per gate)

Desktop-compositor load is present and visible; the baseline sits ~22-26 %
with occasional spikes into the 40-60 % band. Gate samples verbatim:

- pre-run gate: `[23, 23, 24, 23]`
- cpu N=10: `[24, 56, 22, 22]`
- cpu N=30: `[23, 23, 22, 23]`
- cpu N=60: `[22, 58, 23, 23]`
- cpu N=120: `[23, 24, 58, 23]`
- cuda N=10: `[24, 24, 22, 44]`
- cuda N=30: `[24, 22, 23, 58]`
- cuda N=60: `[23, 26, 55, 26]`
- cuda N=120: `[24, 24, 25, 23]`

So: baseline ~23 %, spikes 44-58 % (7 of 36 samples). This is residual desktop
compositor activity, not a dedicated compute job; it is quantified here so the
numbers below can be read with that noise floor in mind.

## Min-of-3 table (runs 1-3; run 0 cold discarded)

"wall" = min wall ms over runs 1-3; "fps"/"wall_fps" are the matched (min-wall)
run's in-window values from the JSON report; "wall/s" = N / wall_s
(full-process rate).

| backend | N   | wall ms | fps (in-window) | wall_fps (in-window) | wall/s (N/wall_s) |
|---------|-----|---------|-----------------|----------------------|-------------------|
| cpu     | 10  | 245.6   | 47.62           | 47.80                | 40.72             |
| cpu     | 30  | 671.9   | 47.24           | 47.31                | 44.65             |
| cpu     | 60  | 1307.1  | 47.36           | 47.35                | 45.90             |
| cpu     | 120 | 2588.6  | 47.11           | 47.11                | 46.36             |
| cuda    | 10  | 378.6   | 149.25          | 149.16               | 26.41             |
| cuda    | 30  | 560.0   | 131.00          | 131.13               | 53.57             |
| cuda    | 60  | 829.2   | 126.85          | 126.72               | 72.36             |
| cuda    | 120 | 1143.0  | 137.46          | 137.59               | 104.99            |

Per-run detail (wall ms): cpu N=10: 266.0/245.6/252.5; cpu N=30:
675.0/688.8/671.9; cpu N=60: 1307.1/1318.9/1366.6; cpu N=120:
2770.5/2588.6/2654.3. cuda N=10: 391.9/416.7/378.6; cuda N=30:
629.1/632.0/560.0; cuda N=60: 1014.9/857.7/829.2; cuda N=120:
1224.7/1487.0/1143.0.

## OLS fits T(N) = T0 + N*t_frame (on min-of-3 wall ms)

| backend | fit          | T0 ms  | t_frame ms | R²     |
|---------|--------------|--------|------------|--------|
| cpu     | primary N≥30 | 31.1   | 21.305     | 1.0000 |
| cpu     | all 4 points | 32.0   | 21.296     | 1.0000 |
| cuda    | primary N≥30 | 403.1  | 6.300      | 0.9788 |
| cuda    | all 4 points | 349.8  | 6.870      | 0.9787 |

CPU is almost perfectly linear with a ~31 ms fixed cost and 21.3 ms/frame.
CUDA has a large fixed cost (~350-400 ms: CUDA context/extractor setup) but a
6.3-6.9 ms/frame slope — i.e. in-window ~145-159 fps vs CPU's ~47 fps. The
crossover (fit) is at N ≈ 21: below it the CUDA cell is setup-dominated.

## Discriminating analysis

The maintainer's two hypotheses for the earlier loaded-run result (CPU
33.1 fps vs CUDA 33.4 fps in-window — statistically indistinguishable):

- (a) external GPU load throttled the CUDA extractors down to the CPU rate
  inside the scoring window;
- (b) something shared (input read/convert) sits inside the loop and caps both
  backends at the same rate.

On the now-quiet GPU the CUDA in-window rates are 126-153 fps across
N in {30, 60, 120} (min-of-3 matched-run fps 131.0 / 126.85 / 137.46), i.e.
**~2.7-2.9x the CPU's ~47 fps** — nowhere near equal. If hypothesis (b) were
true, a shared per-frame serial cost would keep the CUDA in-window rate
pinned at the CPU rate regardless of load; instead the CUDA slope
(6.3 ms/frame) is ~3.4x steeper on the fast side of the CPU's 21.3 ms/frame,
and the gap widens with N exactly as a GPU-vs-CPU compute-rate difference
predicts. The N=10 CUDA cell (26.4 wall/s < CPU 40.7 wall/s) is explained by
the ~400 ms fixed setup, not by a per-frame cap.

**Verdict: hypothesis (a) is supported — strongly.** Under prior load the CUDA
extractors were being crowded down to ~33 fps in-window; with that load gone
they run at ~127-153 fps in-window while CPU stays at its usual ~47 fps
(47.1-47.8 in this window vs 33.1 under the loaded run — the CPU number itself
moved too, so part of the earlier equality was load on the host as well, but
the decisive ~3x CUDA recovery only happens on the GPU side).

Honest caveats: the machine is *quieter*, not idle — the utilization gates
above show a ~22-26 % compositor baseline with 44-58 % spikes (7 of 36
samples), and CUDA run-to-run spread is visible (e.g. cuda N=60 runs
1014.9/857.7/829.2 ms; cuda N=120 1224.7/1487.0/1143.0 ms), consistent with
that residual load grazing the measurements. The R² of the CUDA fit (0.979)
reflects the same noise. Even taking the *worst* timed run of every CUDA cell
(92.74 fps at N=60), CUDA remains ~2x the CPU rate — the conclusion does not
depend on cherry-picking.

## Artifacts

- `C:\PLAN\VMAF\verify\tmp\t0-idle-matrix\summary.json`
- Per-run JSON reports: `C:\PLAN\VMAF\verify\tmp\t0-idle-matrix\n{N}-{backend}-{run}.json` (24 files)
- Harness log: `C:\PLAN\VMAF\verify\tmp\t0_idle_matrix_run.log`
- Harness: `C:\PLAN\VMAF\verify\tmp\t0_idle_matrix.py`
