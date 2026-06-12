# Benchmarks

> **Scope:** this file tracks *fork-added* benchmarks (GPU backends, SIMD
> paths, `--precision` overhead). Netflix's upstream correctness numbers
> are the Netflix golden CPU pools — see [CLAUDE.md §8](../CLAUDE.md).

Runs below are produced by `make bench` (drives `testdata/bench_all.sh`) on
a fixed hardware profile and pinned commit. Contribute new numbers via a
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
[`libvmaf/AGENTS.md` §"Backend-engagement foot-guns"](../libvmaf/AGENTS.md).

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
CC=icx CXX=icpx meson setup core/build libvmaf \
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
