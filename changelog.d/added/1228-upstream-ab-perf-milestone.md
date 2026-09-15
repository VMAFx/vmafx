- **Upstream A/B performance milestone** (`testdata/bench_upstream_ab.py`,
  ADR-1228). Every other benchmark in the tree compares the fork against itself;
  this one builds upstream Netflix/vmaf at a pinned tag and runs both binaries
  over the same fixtures, reporting **speedup** and **score delta** per cell with
  the ADR-1185 measurement discipline. Parity gates the speed number: a run whose
  pooled VMAF moves beyond `--max-score-delta` fails regardless of timing, because
  a speedup bought by changing the score is a regression with a nice number
  attached. Recurring trigger: before every release, on any Renovate bump to CUDA
  / ROCm / oneAPI, on a new upstream release tag, and on any PR whose stated
  purpose is performance — with hardware-generation retuning (target-arch lists,
  occupancy assumptions, ISA dispatch) treated as part of the milestone rather
  than opportunistic work.
- **First measured result, recorded in `docs/benchmarks.md`:** on the inherited
  `vmaf_v0.6.1` path the fork measures **0.989x** against upstream `v3.2.0` —
  parity, which is the right result there since those four features are exactly
  the ones upstream already ships AVX2/AVX-512 for. Across the surface the fork
  *added* SIMD to, it wins substantially: `float_ms_ssim` **4.47x** (upstream has
  no `ms_ssim_decimate` SIMD at all), `psnr_hvs` **1.47x**, `float_ssim`
  **1.46x**. The table reports per-feature rows rather than one geomean,
  precisely because a single number over one model hides both facts.
- **New tracked bug found by the first run** (`T-UPSTREAM-AB-SCORE-DELTA-2026-09-07`):
  the fork's pooled VMAF differs from upstream's by ~`5e-6` using a byte-identical
  model file. All fourteen pooled features agree exactly at the resolution `%.6f`
  exposes; only the final prediction moves, by up to `8e-6` per frame in both
  directions. It sits under the Netflix golden gate's `places=4`, which is why it
  went unnoticed.
