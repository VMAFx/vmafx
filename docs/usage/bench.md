<!-- markdownlint-disable MD013 -->
# `vmaf_bench` — micro-benchmark & validation harness

`vmaf_bench` is a fork-added binary that times individual feature extractors on
pre-staged YUV data and optionally cross-validates GPU output against CPU. It is
**not** a score-producing tool — use the `vmaf` CLI ([cli.md](cli.md)) for
quality assessment. `vmaf_bench` exists purely to:

- compare CPU vs CUDA vs SYCL vs HIP timings per feature,
- validate GPU↔CPU numerical agreement before merging a backend change,
- profile GPU shader breakdowns (SYCL only).

Snapshot benchmark JSONs produced by `vmaf_bench` live under `testdata/` (see
[../architecture/index.md](../architecture/index.md)) and are **not** Netflix
goldens — they are fork-owned. Regenerate with `/regen-snapshots` if you
intentionally moved a baseline.

## Build

```shell
# build with CUDA + SYCL so vmaf_bench can compare all three backends
meson setup build -Denable_cuda=true -Denable_sycl=true
ninja -C build
# binary lives at: build/core/tools/vmaf_bench
```

`vmaf_bench` compiles in every build configuration; CUDA / SYCL rows are
automatically omitted when the respective backend is disabled.

## Test data layout

`vmaf_bench` expects a staging directory (default `/tmp/vmaf_test/`, override
with `--data-dir` or `VMAF_TEST_DATA`):

```text
/tmp/vmaf_test/
├── ref_576x324.yuv      # 48 frames of YUV420P-8
├── dis_576x324.yuv
├── ref_640x480.yuv
├── dis_640x480.yuv
├── ref_1280x720.yuv
├── dis_1280x720.yuv
├── ref_1920x1080.yuv
├── dis_1920x1080.yuv
├── ref_3840x2160.yuv
└── dis_3840x2160.yuv
```

Generate from Big Buck Bunny (or any clip):

```shell
ffmpeg -i bbb.mp4 -frames:v 48 -vf scale=1920:1080 -pix_fmt yuv420p /tmp/vmaf_test/ref_1920x1080.yuv
ffmpeg -i bbb.mp4 -frames:v 48 -vf scale=1920:1080 -pix_fmt yuv420p -c:v rawvideo \
       -x264-params crf=28 /tmp/vmaf_test/dis_1920x1080.yuv
```

## Modes

### Performance benchmark (default)

```shell
vmaf_bench [--resolution WxH] [--frames N] [--bpc N] [--data-dir PATH] [--gpu-only]
```

| Flag | Default | Notes |
| --- | --- | --- |
| `--resolution WxH` | all staged | Restrict to one resolution. |
| `--frames N` | 10 | Max 48 (staged data cap). |
| `--bpc N` | 8 | Bits per component: 8, 10, 12, 16. |
| `--data-dir PATH` | `/tmp/vmaf_test` (or `$VMAF_TEST_DATA`) | Override stage directory. |
| `--gpu-only` | off | Skip CPU feature runs. |
| `--gpu-profile` | off (SYCL-only) | Emit per-shader GPU timing breakdown. |
| `--device N` | auto | Pick GPU device by ordinal (SYCL). |
| `--list-devices` | — | List detected SYCL devices and exit. |

Output is a per-feature, per-backend table of median ms/frame + throughput FPS.

### Validation mode

```shell
vmaf_bench --validate [--resolution WxH] [--frames N]
```

Runs every feature through every compiled backend on the staged data and prints
CPU↔GPU ULP deltas per feature. Used by `/cross-backend-diff` and by reviewers
checking SIMD/GPU PRs.

Target: max absolute difference ≤ 2 ULP for integer features, ≤ 1e-5 relative
for float features. Larger deltas are a regression and should block merge
unless justified inline ([`.github/PULL_REQUEST_TEMPLATE.md`](../../.github/PULL_REQUEST_TEMPLATE.md)
"Cross-backend numerical results").

## Example — single 1080p benchmark

```shell
./build/core/tools/vmaf_bench \
  --resolution 1920x1080 \
  --frames 10 \
  --data-dir /tmp/vmaf_test
```

Expected output (abbreviated):

```text
=== VMAF Benchmark — 1920x1080, 10 frames ===
Feature          CPU(scalar)   CPU(AVX-512)   CUDA         SYCL
integer_vif      112.4 ms        5.1 ms       0.8 ms       1.1 ms
integer_adm       98.2 ms        4.7 ms       0.7 ms       0.9 ms
integer_motion    34.6 ms        1.9 ms       0.3 ms       0.4 ms
...
```

## Example — cross-backend validation

```shell
./build/core/tools/vmaf_bench --validate --resolution 1920x1080 --frames 10
```

Expected output:

```text
=== Cross-backend validation — 1920x1080, 10 frames ===
Feature          CPU vs CUDA      CPU vs SYCL       Verdict
integer_vif      max |Δ| = 1 ULP   max |Δ| = 1 ULP    OK
integer_adm      max |Δ| = 0 ULP   max |Δ| = 0 ULP    OK
integer_motion   max |Δ| = 0 ULP   max |Δ| = 0 ULP    OK
psnr             max |Δ| = 1e-9    max |Δ| = 1e-9     OK
```

## Limitations

- Test data must be pre-staged. `vmaf_bench` does not download anything.
- Resolution list is hard-coded to `576x324`, `640x480`, `1280x720`,
  `1920x1080`, `3840x2160` (per `core/tools/vmaf_bench.c:291-293`).
  Pass `--resolution WxH` to restrict to a single size; adding new
  resolutions requires source changes.
- `--gpu-profile` requires a SYCL build (not wired for CUDA).
- 10 / 12 / 16 bpc (`--bpc`) requires matching test data — staged 8-bit YUVs
  are not auto-converted.

## Related

- [cli.md](cli.md) — the scoring CLI (`vmaf`).
- [../benchmarks.md](../benchmarks.md) — canonical fork benchmark numbers.
- [../backends/index.md](../backends/index.md) — backend compile-time + runtime rules.
- `/cross-backend-diff` skill — wraps `vmaf_bench --validate` with PR-ready formatting.
