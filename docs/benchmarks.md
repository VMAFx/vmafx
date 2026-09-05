# Benchmarks

> **Scope:** this file tracks *fork-added* benchmarks (GPU backends, SIMD
> paths, `--precision` overhead). Netflix's upstream correctness numbers
> are the Netflix golden CPU pools — see [CLAUDE.md §8](../CLAUDE.md).

The **Refreshed per-backend baselines** section below is the current
table; every section after it is retained history, from earlier commits
and earlier models. Historical runs were produced by `make bench`
(which drives `testdata/bench_all.sh`) on a fixed hardware profile and a
pinned commit. Contribute new numbers via a
PR that updates this file alongside the commit that motivates the rerun.

## Hardware profiles

|Profile|CPU|GPUs|Memory|OS|
|---|---|---|---|---|
|`ryzen-4090-arc`|AMD Ryzen 9 9950X3D (16c/32t, Zen 5, AVX-512)|NVIDIA RTX 4090 (24 GB) + Intel Arc A380 (6 GB, fp64-emulated)|96 GB DDR5-6400|Linux 7.0.x (CachyOS)|
|`xeon-arc`|Intel Xeon w9-3475X|Intel Arc A770|128 GB DDR5-4800|Ubuntu 26.04|
|`m4-pro`|Apple M4 Pro|(integrated)|48 GB unified|macOS 15|

The `ryzen-4090-arc` profile is the canonical fork bench host: a single
machine that exposes CUDA (RTX 4090), SYCL (Arc A380 via oneAPI 2025.3
Level Zero), and HIP so all active backends can run back-to-back from one
shell. (Vulkan backend removed per ADR-0726; historical Vulkan bench rows
are preserved below for reference.)

## Refreshed per-backend baselines (2026-09-06, `ryzen-4090-arc`)

Produced by `testdata/bench_backends.py` on commit `cd52f2670`; methodology in
[ADR-1185](adr/1185-backend-perf-baseline-methodology.md), reproduce steps in
[`docs/development/backend-perf-baselines.md`](development/backend-perf-baselines.md).
Every cell is the **median of 3 timed runs after one discarded warmup**,
`--threads 1`, one exclusive backend per run.

> **Read the load column before comparing anything.** These runs shared the
> host with a container rebuild; the 1-minute load average sat between 6.0 and
> 11.5 throughout. Two cells whose `spread` exceeds their difference are not
> different. The 4K CUDA / v0.6.1 cell in particular carries 55 % spread and
> should be treated as an order-of-magnitude figure only.
>
> **GPU rows are incomplete on purpose.** On this commit **no** GPU backend
> completes a scored run on a clip longer than one motion batch — CUDA, SYCL
> and HIP all abort with `problem flushing context`
> (`T-GPU-MOTION-FLUSH-DOUBLE-EMIT-2026-09-06` in
> [`docs/state.md`](state.md)). The CUDA rows below were measured with that
> flush defect patched locally and are therefore **not reproducible from
> `master`**; they are published because the throughput they show is real and
> the retrain planning needs it. SYCL and HIP stay `BLOCKED` because the patch
> does not unblock them.

### `model/vmaf_v0.6.1.json` (the model every historical row above used)

|Fixture|Backend|fps (median)|median s|spread|pooled `vmaf`|keys|load|
|---|---|---|---|---|---|---|---|
|src01 576×324, 48f|`cpu`|**613.71**|0.078|3.5 %|76.667831|15|6.6|
|src01 576×324, 48f|`cuda` (patched)|296.92|0.162|4.0 %|76.682792|14|6.6|
|src01 576×324, 48f|`sycl` / `hip`|BLOCKED|—|—|—|—|—|
|checkerboard 1-px 1080p, 3f|`cpu`|**53.42**|0.056|3.5 %|35.068671|15|6.3|
|checkerboard 1-px 1080p, 3f|`cuda` (patched)|21.07|0.142|0.9 %|35.068667|14|6.3|
|checkerboard 10-px 1080p, 3f|`cpu`|**52.43**|0.057|2.3 %|7.985899|15|6.3|
|checkerboard 10-px 1080p, 3f|`cuda` (patched)|20.48|0.146|2.0 %|7.985899|14|6.0|
|BBB 4K, 200f|`cpu`|14.37|13.917|3.0 %|77.641185|15|6.8|
|BBB 4K, 200f|`cuda` (patched)|**167.16**|1.196|0.8 %|77.641183|14|12.2|

### Default model — no `--model` flag (`vmaf_v1.0.16_3d0h`, ADR-1169)

This is what a caller who names no model actually pays.

|Fixture|Backend|fps (median)|median s|spread|pooled `vmaf`|keys|load|
|---|---|---|---|---|---|---|---|
|src01 576×324, 48f|`cpu`|**379.15**|0.127|1.6 %|82.816062|15|6.3|
|src01 576×324, 48f|`cuda` (patched)|93.20|0.515|0.7 %|82.823783|14|6.3|
|src01 576×324, 48f|`sycl` / `hip`|BLOCKED|—|—|—|—|—|
|checkerboard 1-px 1080p, 3f|`cpu`|**73.79**|0.041|1.4 %|45.315104|15|6.3|
|checkerboard 1-px 1080p, 3f|`cuda` (patched)|12.35|0.243|1.7 %|45.315104|14|6.3|
|checkerboard 10-px 1080p, 3f|`cpu`|**65.31**|0.046|11 %|0.000000|15|6.0|
|checkerboard 10-px 1080p, 3f|`cuda` (patched)|11.78|0.255|3.1 %|0.000000|14|6.0|
|BBB 4K, 200f|`cpu`|**17.52**|11.415|22 %|80.201437|15|9.8|
|BBB 4K, 200f|`cuda` (patched)|9.00|22.222|3.7 %|78.027683|14|12.5|

> **The four 4K cells were re-measured with 5 reps** after the first pass
> returned 55 % spread on the CUDA / v0.6.1 cell. Each 4K row above quotes
> whichever session gave the tighter spread, with that session's own load:
> both CUDA rows are 5-rep, both CPU rows are 3-rep. The 5-rep CPU
> cross-checks agree — 12.31 fps (18.5 % spread, load 13.8) for v0.6.1 and
> 17.80 fps (102 % spread, load 52.6) for the default model, the latter taken
> during a container-rebuild spike and useful only as a sanity check that it
> lands near the 17.52 fps quoted. Cross-session 4K ratios are therefore
> indicative, not tight.

### What the default model costs

The comparison the retrain planning asked for, as a ratio of the two tables
above (same fixture, same backend, same session):

|Fixture|CPU: default vs v0.6.1|CUDA: default vs v0.6.1|
|---|---|---|
|src01 576×324, 48f|**0.62×** (613.71 → 379.15 fps)|**0.31×** (296.92 → 93.20 fps)|
|checkerboard 1-px 1080p, 3f|1.38× (53.42 → 73.79 fps)|0.59× (21.07 → 12.35 fps)|
|checkerboard 10-px 1080p, 3f|1.25× (52.43 → 65.31 fps)|0.58× (20.48 → 11.78 fps)|
|BBB 4K, 200f|1.22× (14.37 → 17.52 fps)|**0.05×** (167.16 → 9.00 fps)|

Three things fall out of this, and only the first is comfortable:

1. **On CPU the default model is not uniformly more expensive.** It costs 38 %
   more per frame on the 576×324 pair but is *faster* on 1080p and 4K content.
   The v1 feature set is not simply "v0.6.1 plus more work" — it trades a
   different mix, and the mix's cost is resolution-dependent.

2. **On CUDA the default model is dramatically worse, and worse the bigger the
   frame gets** — down to 0.05× at 4K, where it is roughly *half the speed
   of the CPU running the same model* (9.00 fps vs 17.52 fps). During
   that run the process
   sat at ~97 % CPU with the RTX 4090 at ~34 % utilisation, which is the
   signature of ADR-1183's twin gating: features whose GPU twin does not
   support the model's options are dispatched to the CPU, so the run pays the
   host↔device transfer cost and then does the work on the host anyway. The
   missing SYCL/HIP AIM device pass
   (`T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05`) is the same class
   of gap on the other two backends.

3. **GPU offload only pays at 4K, and only for v0.6.1.** Every 1080p×3f cell
   has CUDA losing to CPU by 2.5–5×, which is expected — three frames cannot
   amortise context creation — but the 4K default-model row shows the loss
   persisting into a workload that should be firmly GPU-favourable.

The retrain planning number, stated plainly: **on this host the default model
costs 1.6× the CPU time of `v0.6.1` on the 576×324 golden pair, and on CUDA it
gives up the entire GPU speedup — 4K throughput falls from 167.16 fps to
9.00 fps, well below the CPU's own 17.52 fps.**

## Backend comparison (Netflix normal pair, 576×324, 48 frames)

Source: `python/test/resource/yuv/src01_hrc00_576x324.yuv` vs `…hrc01…`.
Model: `model/vmaf_v0.6.1.json`. Threads: 1. Precision: CLI default
`%.6f` per [ADR-0119](adr/0119-cli-precision-default-revert.md).
Numbers averaged over 5 wall-clock reps after 1 warmup; standard
deviation in parentheses. Commit `41301496` on `ryzen-4090-arc`.

|Backend|fps (higher better)|wall ms / 48f|vmaf pool|metrics-keys|delta vs CPU pool|
|---|---|---|---|---|---|
|`cpu` (full ISA, AVX-512)|598 (±21)|80.3|`76.667828`|15|0 (reference)|
|`cuda` (RTX 4090)|278 (±52)|177.5|`76.667828`|12|0.0 — pool match to 6 dp; per-frame max ULP diff 1.8×10⁻⁵|
|`sycl` (Arc A380)|315 (±0.9)|152.3|`76.667767`|34|-6.1×10⁻⁵ pool; per-frame max diff 1.11×10⁻³|
|~~`vulkan`~~ (removed ADR-0726)|(historical: 171 fps)|(historical: 280.6ms)|`76.667758`|34|historical reference only|

**Key-count check.** Each backend emits a different `frames[0].metrics`
key set (CPU=15 with `integer_aim`/`integer_motion3`/`integer_adm3`,
CUDA=12, SYCL=34 with raw `_num`/`_den` intermediates). Identical
key counts across two rows would indicate a silent-fallback to CPU; the
counts above confirm each backend actually engaged. See
[`AGENTS.md` §"Backend-engagement foot-guns"](../AGENTS.md).

## Backend comparison (1080p, 5 frames)

Source: `python/test/resource/yuv/src01_hrc{00,01}_1920x1080_5frames.yuv`.
Same setup as 576×324.

|Backend|fps|wall ms / 5f|vmaf pool|metrics-keys|
|---|---|---|---|---|
|`cpu`|45.6 (±1.0)|109.7|`35.815478`|15|
|`cuda`|33.6 (±1.1)|148.8|`35.815478`|12|
|`sycl`|41.1 (±0.7)|121.7|`35.815404`|34|
|~~`vulkan`~~ (removed ADR-0726)|(historical: 21.8 fps)|(historical: 229.5ms)|`35.815399`|34|

CPU outpaces CUDA at 1080p × 5 frames because dispatch overhead
dominates the workload — only 5 frames doesn't amortise the CUDA
launch/copy cost. CUDA decisively wins once the workload grows (see 4K
below).

## Backend comparison (BBB 4K, 200 frames)

Source: `testdata/bbb/{ref,dis}_3840x2160_200f.yuv` (BigBuckBunny 4K
master, ffmpeg-encoded ref + libx264 CRF=35 round-trip distortion; see
[How to reproduce](#how-to-reproduce)).

|Backend|fps|wall s / 200f|vmaf pool|speedup vs CPU|
|---|---|---|---|---|
|`cpu`|13.9 (±0.5)|14.43|`36.343813`|1.0× (baseline)|
|`cuda` (RTX 4090)|**227.6** (±11.3)|0.88|`36.343815`|**16.4×**|
|`sycl` (Arc A380)|32.1 (±0.1)|6.23|`36.343780`|2.3×|
|~~`vulkan`~~ (removed ADR-0726)|(historical: 14.1 fps)|(historical: 14.16s)|`36.343774`|historical|

Notes:

- **CUDA at 4K** is the headline number — the RTX 4090 sustains 227 fps
  on 8-bit 3840×2160 with `vmaf_v0.6.1.json`, ~16× faster than the
  CPU + AVX-512 baseline.
- **SYCL on Arc A380** is fp64-emulated (the A380 is a Gen12.7 part
  without native fp64). The 2.3× headline understates the SIMD path's
  potential on a fp64-native dGPU; revisit when an Arc B-series or
  Battlemage host lands. See backlog T7-17.
- **Vulkan rows** are historical; the backend was removed in ADR-0726.
  The performance bottleneck (dispatch-overhead-bound on NVIDIA, 14 fps
  matching CPU) contributed to the removal rationale.

## CPU SIMD-ISA breakdown (576×324)

Selected via `--cpumask` (bits set = ISAs to *disable*).

|Configuration|`--cpumask`|fps|wall ms / 48f|speedup vs scalar|
|---|---|---|---|---|
|Scalar (no SIMD)|`63`|92.4 (±0.8)|519.6|1.0×|
|Up to AVX2|`48`|273.5 (±0.8)|175.5|2.96×|
|Default (full, AVX-512)|`0`|611.5 (±8.9)|78.5|**6.62×**|

AVX-512 over AVX2 buys another 2.24× on top of the AVX2 baseline on the
9950X3D (Zen 5 has 512-bit SIMD pipes). Pools match across all three
configurations to within `assertAlmostEqual(places=6)` per the Netflix
golden gate.

## `--precision` overhead (576×324 CPU, 48 frames)

String formatting is not on the hot path; switching from `%.6f`
(default per [ADR-0119](adr/0119-cli-precision-default-revert.md)) to
`%.17g` (`--precision=max`) changes only the JSON-emit stage.

|`--precision`|fps|wall ms|JSON output size|size delta vs default|
|---|---|---|---|---|
|no flag (`%.6f` default)|613.8 (±6.7)|78.2|31 837 B|baseline|
|`=6` (explicit)|616.9 (±8.3)|77.8|31 525 B|-1.0 %|
|`=max` (`%.17g`)|612.8 (±11.2)|78.4|40 041 B|**+25.8 %**|

Wall-time delta is in the noise (<1 % across all three), confirming
that the per-frame cost of `%.17g` is negligible — the cost shows up in
JSON byte-count, not in wall time. Use `--precision=max` whenever
cross-backend numerical diffing or IEEE-754 round-trip determinism
matters.

## How to reproduce

```bash
# 1. Build with all backends (oneAPI 2025.3 sourced for icx/icpx + Arc visibility)
source /opt/intel/oneapi-2025.3/setvars.sh
CC=icx CXX=icpx meson setup core/build core \
    -Denable_cuda=true -Denable_sycl=true \
    -Db_lto=false --buildtype=release
# Note: -Denable_vulkan=enabled removed per ADR-0726
ninja -C core/build

# 2. Acquire fixtures (gitignored — don't commit)
#    a) Netflix golden 576x324 + 1080p_5frames already live in
#       python/test/resource/yuv/ in the main checkout.
#    b) BBB 4K 200-frame pair: download from archive.org and ffmpeg-encode.
mkdir -p testdata/bbb
curl -L https://archive.org/download/big-buck-bunny-4k-60fps/BigBuckBunny4k60fps.mp4 \
    -o /tmp/bbb4k.mp4
ffmpeg -y -i /tmp/bbb4k.mp4 -frames:v 200 -pix_fmt yuv420p -s 3840x2160 \
    testdata/bbb/ref_3840x2160_200f.yuv
ffmpeg -y -i /tmp/bbb4k.mp4 -frames:v 200 -c:v libx264 -crf 35 -preset veryfast \
    -pix_fmt yuv420p -s 3840x2160 -f rawvideo - \
    | ffmpeg -y -f rawvideo -pix_fmt yuv420p -s 3840x2160 -i - \
        -pix_fmt yuv420p -s 3840x2160 testdata/bbb/dis_3840x2160_200f.yuv
#    Or any equivalent libx264 CRF=35 round-trip; the absolute pool drifts
#    with codec parameters but the fps numbers don't.

# 3. Run the bench
VMAF_BIN="$(pwd)/core/build/tools/vmaf" bash testdata/bench_all.sh

# 4. Verify each backend engaged via per-row metrics-key counts in the
#    bench output ("CPU 15 keys, CUDA 12 keys, SYCL 34 keys").
#    Identical key counts across two rows = silent CPU fallback.
```

For SIMD breakdown / `--precision` overhead numbers, see the harness
scripts under `testdata/` (or paste the inlined `repeat_bench.py` /
`simd_bench.py` / `precision_bench.py` from the
[T7-37 PR description](https://github.com/VMAFx/vmafx/pulls?q=T7-37)).

## FFmpeg lavfi performance harness

`testdata/bench_perf.py` runs the FFmpeg filter path used by the historical
`perf_benchmark_results.json` snapshot. It is useful when the question is
"how fast is FFmpeg decode/upload/filter end-to-end?" rather than "how fast
is the `vmaf` CLI binary?"

The harness is portable across checkouts:

```bash
python3 testdata/bench_perf.py \
    --ffmpeg /path/to/ffmpeg \
    --backend cpu \
    --backend cuda \
    --runs 3
```

Environment overrides are also supported:

| Variable | CLI equivalent | Purpose |
| --- | --- | --- |
| `VMAF_FFMPEG` | `--ffmpeg` | FFmpeg binary with the fork's libvmaf filters. |
| `VMAF_BENCH_RUNS` | `--runs` | Timing repetitions per backend. |
| `VMAF_BENCH_TIMEOUT_S` | `--timeout-s` | Per-run timeout. |
| `VMAF_BBB_MP4_REF` | `--bbb-mp4-ref` | Optional external BBB 4K MP4 for the decode+VMAF test. |
| `VMAF_SYCL_DEVICE` | `--sycl-device` | VAAPI render node used by the SYCL/QSV import path. |
| `VMAF_LD_LIBRARY_PATH` | `--ld-library-path` | Runtime library path for FFmpeg/libvmaf. |

The committed raw 4K BBB pair remains the required fixture. The 1080p raw pair
and MP4 decode test are optional by default; pass `--require-all` when you want
a strict lab run that fails on any missing configured input. Use `--list-tests`
to audit fixture availability and `--dry-run` to print the exact FFmpeg
commands without touching hardware.
