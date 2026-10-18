# Benchmarks

This page records fork-added benchmark results (GPU backends, SIMD paths,
`--precision` overhead). Every table carries the date, host, commit and
command of its measurement; tables that lack them are collected at the end
under [Unattributed history](#unattributed-history-do-not-cite). Start with
the headline table.

!!! note "Scope"
    Netflix's upstream correctness numbers are the Netflix golden CPU pools;
    see [AGENTS.md §8](https://github.com/VMAFx/vmafx/blob/master/AGENTS.md).

## Current headline numbers

| Result | Value | Measured | Details |
| --- | --- | --- | --- |
| Inherited path (`vmaf_v0.6.1`, CPU) against upstream `v3.2.0` | parity: 0.989x on the 48f 1080p fixture; scores identical since ADR-1475 | 2026-09-07 (speed), 2026-10-02 (scores) | [Upstream A/B](#upstream-ab-adr-1228) |
| `float_ms_ssim` against upstream | 4.47x faster (1.318 s against 5.894 s) | 2026-09-07 | [Where the fork wins](#where-the-fork-actually-wins-features-upstream-left-scalar) |
| CUDA against CPU at BBB 4K, 200 frames, `vmaf_v0.6.1` | 167.16 fps against 14.37 fps (CUDA row patched locally) | 2026-09-06, `cd52f2670` | [Baselines](#refreshed-per-backend-baselines-2026-09-06-reference-workstation) |
| Default model against `vmaf_v0.6.1`, CPU, 576x324 | 379.15 fps against 613.71 fps (0.62x) | 2026-09-06, `cd52f2670` | [Default model cost](#what-the-default-model-costs) |
| Default model on CUDA at 4K | 9.00 fps, below the CPU's 17.52 fps | 2026-09-06, `cd52f2670` | [Default model cost](#what-the-default-model-costs) |

!!! note "Scores quoted in the tables"
    The pooled scores in the 2026-09-06 tables (76.667831 for `vmaf_v0.6.1`,
    82.816062 for the default model) are the values of that session. Current
    `master` reports 82.816060 for the default model on the same pair, after
    the SpEED fix of ADR-1477.

## Upstream A/B (ADR-1228)

This is the only section that compares the fork against **upstream
Netflix/vmaf at a pinned commit**. It answers whether the fork is worth using
on throughput grounds, and whether it stayed exact while getting there. Every
other table on this page compares the fork against itself.

**CPU path only.** Upstream has no SYCL, HIP or Metal backend and its CUDA
backend covers a different feature set, so a GPU comparison would measure the
hardware rather than the work. Per-backend numbers stay in
[`backend-perf-baselines.md`](development/backend-perf-baselines.md).

### Harness

[`testdata/bench_upstream_ab.py`](../testdata/bench_upstream_ab.py) builds
upstream at the recorded parity head and this tree with the golden build
profile, both through the [upstream parity
guard](development/upstream-parity.md)
([ADR-1487](adr/1487-upstream-parity-policy-and-guard.md)). It then runs both
binaries over the same fixtures with the ADR-1185 discipline:

- one discarded warmup,
- `--runs` timed repetitions reported as the median,
- spread and load average recorded.

The score verdict is the guard's: the model's values at `%.17g` on the same
fixtures against the allowlist of recorded deviations. The "Score delta"
column below is the six-decimal pooled difference of the two tools and is
informational.

```bash
testdata/bench_upstream_ab.py --runs 5 --json /tmp/ab.json
testdata/bench_upstream_ab.py --upstream-ref v3.2.0   # another upstream commit or tag
```

To check score equality with upstream, run the guard (`make upstream-parity`,
see [upstream parity guard](development/upstream-parity.md)); the harness has
no score-delta flag.

### 2026-09-07 — fork `257ac1ec4` vs upstream `v3.2.0`, single-threaded

Measured with the six-decimal ceiling the harness had before the guard.

| Fixture | Upstream | Fork | Speedup | Score delta |
| --- | --- | --- | --- | --- |
| 48f 1920x1080 (concatenated checkerboard) | 0.756 s | 0.764 s | **0.989x** | `+5.0e-06` |
| `src01_576x324`, 48f | 0.073 s | 0.072 s | 1.01x † | `0` |
| `checkerboard_1px`, 3f | 0.053 s | 0.054 s | 0.97x † | `+4.0e-06` |
| `checkerboard_10px`, 3f | 0.053 s | 0.054 s | 0.98x † | `0` |

!!! warning "† Startup-dominated: do not quote these"
    Every tracked fixture runs in well under a second, so process startup,
    model parse and JSON emit dominate the wall clock and the ratio sits near
    1.00x by construction. Two consecutive runs of identical code produced
    geomeans of 1.05x and 0.99x. The harness warns when this is the case;
    `MIN_USEFUL_SECONDS` encodes the threshold. The first row is the only one
    long enough to mean anything, and it needs a locally built fixture
    because the 4K pair is untracked.

### Reading the result

On the *inherited* path (the `vmaf_v0.6.1` model, whose four features are
`integer_adm`, `integer_vif`, `integer_motion` and `integer_aim`) the fork is
at parity with upstream.

- **What is equal:** the fork has not regressed the code it inherited. That
  is the right result to want there.
- **What it does not say:** nothing about the fork's speed in general.
  Upstream already ships AVX2 and AVX-512 for exactly those four features, so
  picking that model picks the one workload where both projects are equally
  optimised.
- **Both statements are true:** the fork has not made the inherited integer
  path faster, and it is substantially faster across the surface it added
  SIMD to. A single geomean over one model would have hidden both facts,
  which is why the next section reports per-feature rows.

### 2026-10-02 — score delta closed (ADR-1475)

The score deltas of the 2026-09-07 table were one expression: the
quantisation step of integer ADM formed its exponent in `double` where Netflix
forms it in `float`
([ADR-1475](adr/1475-integer-adm-quant-step-upstream-float.md)). With
Netflix's expression restored, the score comparison passes against Netflix
master `cea2b4d8` on all four fixtures, the 200-frame 3840x2160 clip
included. Before the fix the delta was `4.0e-06` on the 1 px checkerboard pair
and `1.0e-06` on the 4K clip. No timing was recorded in that run. The
tracking row `T-UPSTREAM-AB-SCORE-DELTA-2026-09-07` in [`state.md`](state.md)
is closed.

### Where the fork actually wins: features upstream left scalar

Measured in the 2026-09-07 session of the table above (fork `257ac1ec4`
against upstream `v3.2.0`, a reference workstation, per
[ADR-1228](adr/1228-upstream-ab-perf-milestone.md)): the same 48-frame
1920x1080 pair, single-threaded, `--feature X --no_prediction`. The fork adds
x86 SIMD sources upstream does not have.

| Feature | Upstream | Fork | Speedup |
| --- | --- | --- | --- |
| `float_ms_ssim` | 5.894 s | 1.318 s | **4.47x** |
| `psnr_hvs` | 1.182 s | 0.801 s | **1.47x** |
| `float_ssim` | 0.358 s | 0.246 s | **1.46x** |
| `ciede` | 13.300 s | 12.816 s | 1.04x |
| `float_moment` | 0.191 s | 0.191 s | 1.00x |
| `float_adm` | 0.715 s | 0.738 s | 0.97x |
| `cambi` | 1.012 s | 1.117 s | 0.91x |
| `psnr` | 0.024 s | 0.026 s | 0.91x † |

† startup-dominated at 24 ms; not a real measurement.

`float_ms_ssim` is the headline: upstream has no `ms_ssim_decimate` SIMD at all,
so it runs the decimation scalar. `psnr_hvs` and `float_ssim` are the same story
at smaller scale. The rows near 1.00x are where upstream is already vectorised
or
where the kernel is memory-bound.

Features that exist only in the fork (for example `speed_qa`, `delta_e_itp`,
`niqe`, `brisque`, `pu21`, `ssimulacra2` and the tiny-AI extractors) have no
upstream counterpart to compare against and are therefore absent from both
tables.

## Hardware profiles

|Profile|CPU|GPUs|Memory|OS|
|---|---|---|---|---|
|a reference workstation|AMD Ryzen 9 9950X3D (16c/32t, Zen 5, AVX-512)|NVIDIA RTX 4090 (24 GB) + Intel Arc A380 (6 GB, fp64-emulated)|96 GB DDR5-6400|Linux 7.0.x (CachyOS) at the 2026-09 measurements|
|`xeon-arc`|Intel Xeon w9-3475X|Intel Arc A770|128 GB DDR5-4800|Ubuntu 26.04|
|`m4-pro`|Apple M4 Pro|(integrated)|48 GB unified|macOS 15|

The reference workstation profile is the canonical fork bench host: a single
machine that exposes CUDA (RTX 4090), SYCL (Arc A380 via oneAPI Level Zero)
and HIP, so all active backends can run back-to-back from one shell. The
`xeon-arc` and `m4-pro` profiles are defined for future runs; no table on
this page uses them. The Vulkan backend was removed per ADR-0726; historical
Vulkan bench rows are preserved in the history sections for reference.

## Refreshed per-backend baselines (2026-09-06, reference workstation)

Produced by `testdata/bench_backends.py` on commit `cd52f2670`; methodology in
[ADR-1185](adr/1185-backend-perf-baseline-methodology.md), reproduce steps in
[`docs/development/backend-perf-baselines.md`](development/backend-perf-baselines.md).
Every cell is the **median of 3 timed runs after one discarded warmup**,
`--threads 1`, one exclusive backend per run.

!!! warning "Read the load column before comparing anything"
    These runs shared the host with a container rebuild; the 1-minute load
    average sat between 6.0 and 11.5 throughout. Two cells whose `spread`
    exceeds their difference are not different. The 4K CUDA / v0.6.1 cell in
    particular carries 55 % spread and should be treated as an
    order-of-magnitude figure only.

!!! warning "GPU rows are incomplete on purpose"
    On this commit **no** GPU backend completes a scored run on a clip longer
    than one motion batch: CUDA, SYCL and HIP all abort with
    `problem flushing context`
    (`T-GPU-MOTION-FLUSH-DOUBLE-EMIT-2026-09-06` in
    [`docs/state.md`](state.md)). The CUDA rows below were measured with that
    flush defect patched locally and are therefore **not reproducible from
    `master`**. They are published because the throughput they show is real
    and the retrain planning needs it. SYCL and HIP stay `BLOCKED` because the
    patch does not unblock them.

!!! note "Cross-check that the CPU rows are `master` behaviour"
    Re-running the 576x324 cells against a *pristine* all-backends build of
    the same commit reproduces the pooled scores exactly (76.667831 and
    82.816062). Throughput lands at 551.24 and 349.46 fps against the 613.71
    and 379.15 quoted below. The gap of about 11 % is the load difference
    (12.6 against about 6.5) and a different build directory, not a code
    difference. It is a fair estimate of how much this host's numbers move
    between sessions.

### `model/vmaf_v0.6.1.json` (the model every historical row below used)

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

!!! note "The four 4K cells were re-measured with 5 reps"
    The first pass returned 55 % spread on the CUDA / v0.6.1 cell. Each 4K row
    above quotes whichever session gave the tighter spread, with that session's
    own load: both CUDA rows are 5-rep, both CPU rows are 3-rep. The 5-rep CPU
    cross-checks agree: 12.31 fps (18.5 % spread, load 13.8) for v0.6.1 and
    17.80 fps (102 % spread, load 52.6) for the default model. The latter was
    taken during a container-rebuild spike and is a sanity check only.
    Cross-session 4K ratios are therefore indicative, not tight.

### What the default model costs

The comparison the retrain planning asked for, as a ratio of the two tables
above (same fixture, same backend, same session):

|Fixture|CPU: default vs v0.6.1|CUDA: default vs v0.6.1|
|---|---|---|
|src01 576×324, 48f|**0.62×** (613.71 → 379.15 fps)|**0.31×** (296.92 → 93.20 fps)|
|checkerboard 1-px 1080p, 3f|1.38× (53.42 → 73.79 fps)|0.59× (21.07 → 12.35 fps)|
|checkerboard 10-px 1080p, 3f|1.25× (52.43 → 65.31 fps)|0.58× (20.48 → 11.78 fps)|
|BBB 4K, 200f|1.22× (14.37 → 17.52 fps)|**0.05×** (167.16 → 9.00 fps)|

### Reading the ratios

Three things fall out of this, and only the first is comfortable:

1. **On CPU the default model is not uniformly more expensive.** It costs 38 %
   more per frame on the 576×324 pair but is *faster* on 1080p and 4K content.
   The v1 feature set is not simply "v0.6.1 plus more work" — it trades a
   different mix, and the mix's cost is resolution-dependent.

2. **On CUDA the default model is dramatically worse, and worse the bigger the
   frame gets.** It falls to 0.05× at 4K, where it is roughly *half the speed
   of the CPU running the same model* (9.00 fps vs 17.52 fps).

    During that run the process sat at ~97 % CPU with the RTX 4090 at ~34 %
    utilisation. This is the signature of ADR-1183's twin gating: features
    whose GPU twin does not support the model's options are dispatched to the
    CPU, so the run pays the host↔device transfer cost and then does the
    work on the host anyway.

    SYCL and HIP had the same class of gap in their ADM twins until
    [ADR-1362](adr/1362-sycl-integer-adm-aim-device-pass.md) and
    [ADR-1525](adr/1525-adm-hip-aim-device-pass.md) gave them their own AIM
    passes.

3. **GPU offload only pays at 4K, and only for v0.6.1.** Every 1080p×3f cell
   has CUDA losing to CPU by 2.5–5×, which is expected — three frames cannot
   amortise context creation — but the 4K default-model row shows the loss
   persisting into a workload that should be firmly GPU-favourable.

### The retrain planning number

Stated plainly: **on this host the default model costs 1.6× the CPU time of
`v0.6.1` on the 576×324 golden pair, and on CUDA it
gives up the entire GPU speedup — 4K throughput falls from 167.16 fps to
9.00 fps, well below the CPU's own 17.52 fps.**

## How to reproduce

The commands below reproduce the dated tables of the history sections; the
baselines above come from `testdata/bench_backends.py` (see
[backend-perf-baselines](development/backend-perf-baselines.md)).

1. Build with all backends, with oneAPI sourced for `icx`/`icpx` and Arc
   visibility.

    ```bash
    source /opt/intel/oneapi/setvars.sh
    CC=icx CXX=icpx meson setup core/build core \
        -Denable_cuda=true -Denable_sycl=true \
        -Db_lto=false --buildtype=release
    ninja -C core/build
    ```

2. Acquire the fixtures (gitignored; do not commit them). The Netflix golden
   576x324 and 1080p_5frames pairs already live in `python/test/resource/yuv/`
   in the main checkout. For the BBB 4K 200-frame pair, download the source
   from archive.org and encode the reference.

    ```bash
    mkdir -p testdata/bbb
    curl -L https://archive.org/download/big-buck-bunny-4k-60fps/BigBuckBunny4k60fps.mp4 \
        -o /tmp/bbb4k.mp4
    ffmpeg -y -i /tmp/bbb4k.mp4 -frames:v 200 -pix_fmt yuv420p -s 3840x2160 \
        testdata/bbb/ref_3840x2160_200f.yuv
    ```

3. Encode the distorted 4K clip (libx264 CRF 35 round trip). Any equivalent
   round trip works; the absolute pool drifts with codec parameters but the
   fps numbers do not.

    ```bash
    ffmpeg -y -i /tmp/bbb4k.mp4 -frames:v 200 -c:v libx264 -crf 35 -preset veryfast \
        -pix_fmt yuv420p -s 3840x2160 -f rawvideo - \
        | ffmpeg -y -f rawvideo -pix_fmt yuv420p -s 3840x2160 -i - \
            -pix_fmt yuv420p -s 3840x2160 testdata/bbb/dis_3840x2160_200f.yuv
    ```

4. Run the bench.

    ```bash
    VMAF_BIN="$(pwd)/core/build/tools/vmaf" bash testdata/bench_all.sh
    ```

5. Verify that each backend engaged, from the per-row emitted key count, the
   pool, the throughput and stderr. A GPU key count equal to the CPU's is a
   fallback warning; never compare against a frozen expected count.

For the SIMD breakdown and `--precision` overhead numbers, see the harness
scripts under `testdata/` (or the inlined `repeat_bench.py` / `simd_bench.py`
/ `precision_bench.py` from the
[T7-37 PR description](https://github.com/VMAFx/vmafx/pulls?q=T7-37)).

### Quick CLI smoke benchmark

`testdata/bench_quick.py` runs the CPU CLI three times for every complete
`ref_<width>x<height>_48f.yuv` / `dis_<width>x<height>_48f.yuv` pair it finds.
It reports best and average fps without writing a snapshot:

```bash
VMAF_BIN="$(pwd)/core/build/tools/vmaf" \
VMAF_TESTDATA="$(pwd)/testdata" \
python3 testdata/bench_quick.py
```

Each Git lookup is limited to 10 seconds and each VMAF run to 300 seconds.
The command exits 1 on a VMAF failure or timeout, exits 2 when no complete
fixture pair exists, and prints the captured diagnostic on stderr. Missing
resolutions are skipped; a reference without its matching distorted input is
not treated as a runnable pair.

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

## History

Older runs, retained for reference. Do not use them as current numbers.

### Historical backend comparison (Netflix normal pair, 576x324, 48 frames)

Source: `python/test/resource/yuv/src01_hrc00_576x324.yuv` vs `…hrc01…`.
Model: `model/vmaf_v0.6.1.json`. Threads: 1. Precision: CLI default
`%.6f` per [ADR-0119](adr/0119-cli-precision-default-revert.md).
Numbers averaged over 5 wall-clock reps after 1 warmup; standard
deviation in parentheses. Commit `41301496` on a reference workstation.

|Backend|fps (higher better)|wall ms / 48f|vmaf pool|metrics-keys|delta vs CPU pool|
|---|---|---|---|---|---|
|`cpu` (full ISA, AVX-512)|598 (±21)|80.3|`76.667828`|15|0 (reference)|
|`cuda` (RTX 4090)|278 (±52)|177.5|`76.667828`|12|0.0 — pool match to 6 dp; per-frame max ULP diff 1.8×10⁻⁵|
|`sycl` (Arc A380)|315 (±0.9)|152.3|`76.667767`|34|-6.1×10⁻⁵ pool; per-frame max diff 1.11×10⁻³|
|~~`vulkan`~~ (removed ADR-0726)|(historical: 171 fps)|(historical: 280.6ms)|`76.667758`|34|historical reference only|

**Key-count check for this dated run.** At commit `41301496`, the emitted
sets were CPU=15 (including `integer_aim`/`integer_motion3`/`integer_adm3`),
CUDA=12, and SYCL=34 (including raw `_num`/`_den` intermediates). Those
values document this run; they are not permanent backend contracts. On
2026-09-06 at `cd52f2670`, the FFmpeg filter path instead emitted CPU=15,
CUDA=14, and SYCL=24. The harness records actual counts and flags equality
with CPU as a fallback signal to corroborate with pool and throughput. See
[`core/AGENTS.md` §"Backend-engagement foot-guns"](../core/AGENTS.md).

## Unattributed history (do not cite)

The tables below carry no date, commit, host profile or command of their own.
They are kept because they show the shape of the results, not as measurements
to quote. Where a source is stated, it is the assumption the original text
made.

### Backend comparison (1080p, 5 frames)

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

### Backend comparison (BBB 4K, 200 frames)

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

### CPU SIMD-ISA breakdown (576×324)

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

### `--precision` overhead (576×324 CPU, 48 frames)

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
