<!-- markdownlint-disable MD013 -->
# `vmaf_bench` — micro-benchmark and validation harness

`vmaf_bench` times individual feature extractors on pre-staged YUV data and can
cross-validate GPU output against the CPU. It is **not** a score-producing
tool; use the `vmaf` CLI ([cli.md](cli.md)) for quality assessment. It exists
to:

- compare CPU vs CUDA vs SYCL timings per feature,
- validate GPU to CPU numerical agreement before merging a backend change,
- profile GPU shader breakdowns (SYCL only).

`vmaf_bench` has no HIP or Metal path. Snapshot benchmark JSONs produced by
the benchmark harness live under `testdata/` (see
[../architecture/index.md](../architecture/index.md)) and are fork-owned, not
Netflix goldens. Regenerate them with `/regen-snapshots` if you intentionally
move a baseline.

## Quick start

1. Build with the backends you want to compare. The Meson source directory is
   `core/`:

    ```shell
    meson setup build core -Denable_cuda=true -Denable_sycl=true
    ninja -C build
    ```

    The binary is `build/tools/vmaf_bench`. It compiles in every
    configuration; CUDA and SYCL rows are omitted when that backend is disabled.

2. Stage test data (see [Test data layout](#test-data-layout)).
3. Run a benchmark:

    ```shell
    ./build/tools/vmaf_bench --resolution 1920x1080 --frames 10 --data-dir /tmp/vmaf_test
    ```

## Test data layout

`vmaf_bench` expects a staging directory (default `/tmp/vmaf_test/`, override
with `--data-dir` or `VMAF_TEST_DATA`) holding 8-bit YUV 4:2:0 pairs:

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

Generate them from Big Buck Bunny (or any clip):

```shell
ffmpeg -i bbb.mp4 -frames:v 48 -vf scale=1920:1080 -pix_fmt yuv420p /tmp/vmaf_test/ref_1920x1080.yuv
ffmpeg -i bbb.mp4 -frames:v 48 -vf scale=1920:1080 -pix_fmt yuv420p -c:v libx264 -crf 28 -f h264 - \
  | ffmpeg -f h264 -i - -pix_fmt yuv420p /tmp/vmaf_test/dis_1920x1080.yuv
```

The distorted clip goes through a real libx264 encode and back to raw YUV: writing
`-c:v rawvideo` with `-x264-params` encodes nothing, FFmpeg ignores the option, and the
"distorted" file is byte for byte the reference.

`vmaf_bench` does not download anything. The repository's
`testdata/ref_576x324_48f.yuv` and `dis_576x324_48f.yuv` can be copied in under
the `ref_576x324.yuv` / `dis_576x324.yuv` names for a quick smoke run.

## Performance benchmark (default mode)

```shell
vmaf_bench [--resolution WxH] [--frames N] [--bpc N] [--data-dir PATH] [--gpu-only]
```

### Benchmark flags

| Flag | Default | Notes |
| --- | --- | --- |
| `--resolution WxH` | all staged | Restrict to one resolution. |
| `--frames N` | 10 | Max 48 (staged data cap). |
| `--bpc N` | 8 | Bits per component: 8, 10, 12, 16. Needs matching test data; 8-bit YUVs are not converted. |
| `--data-dir PATH` | `/tmp/vmaf_test` (or `$VMAF_TEST_DATA`) | Staging directory. |
| `--gpu-only` | off | Skip CPU feature runs. |

### Device selection flags

| Flag | Default | Notes |
| --- | --- | --- |
| `--device N` | auto | Pick the GPU by ordinal (SYCL). |
| `--list-devices` | | List detected SYCL devices and exit. In a build without a GPU backend it prints `No GPU backend enabled`. |

### Profiling flag

| Flag | Default | Notes |
| --- | --- | --- |
| `--gpu-profile` | off | Print a per-shader GPU timing breakdown. Requires a SYCL build; it is not wired for CUDA. It returns non-zero when frame submission or the final flush fails. |

### Output

The output is a header (version, data directory, frames per test) followed by
one row per feature and backend with the average time per frame and the
throughput. Times are averages, not medians. The CPU rows come from the
`motion`, `vif`, `adm`, `float_ssim`, `float_ms_ssim` and `psnr` extractors;
GPU rows appear for each compiled backend. This is a 576x324 CPU-only run:

```text
VMAF Performance Benchmark (v1.0.0-rc.2-310-g1b1663830)
Data: /tmp/vmaf_test
Frames per test: 10

Feature                            Res   Init ms    Avg ms  Total ms       FPS
----------------------------------------------------------------------------------------
motion (CPU)                   576x324       2.5      0.14       1.2    7337.4
vif (CPU)                      576x324       0.2      0.97       8.7    1034.8
adm (CPU)                      576x324       0.1      0.33       2.9    3051.9
float_ssim (CPU)               576x324       0.1      1.21      10.9     827.4
float_ms_ssim (CPU)            576x324       0.1      1.80      16.2     554.9
psnr (CPU)                     576x324       0.1      0.05       0.4   21865.3
```

## Validation mode

```shell
vmaf_bench --validate [--resolution WxH] [--frames N]
```

Validation compares the `motion`, `vif` and `adm` scores of each compiled GPU
backend against the CPU, frame by frame, on the staged data. Each pair has an
absolute tolerance:

| Feature | Tolerance (absolute) | Scores compared |
| --- | --- | --- |
| `motion` | `5e-6` | the integer motion scores (CUDA also `motion`; SYCL only `motion2`) |
| `vif` | `1e-3` | the four VIF scale scores |
| `adm` | `0.5` | `adm2` and the four ADM scale scores |

Without a GPU backend it prints `No GPU backend enabled, cannot validate`. With
one, the output starts with `VMAF GPU Correctness Validation` and has one line
per score (the values below are illustrative):

```text
  Motion/CU  @ 1920x1080  VMAF_integer_feature_motion2_score        max_diff=0.00e+00  [PASS]
```

The status is `PASS`, `FAIL` (difference above the tolerance) or `NaN!`. The run
ends with `ALL PASSED` or `FAILURES`. A larger delta is a regression and should
block merge unless justified inline
([`.github/PULL_REQUEST_TEMPLATE.md`](../../.github/PULL_REQUEST_TEMPLATE.md),
"Cross-backend numerical results"). The `/cross-backend-diff` skill wraps this
mode with PR-ready formatting.

An extractor setup, frame submission or final flush failure is reported as a
`SKIP` for that CPU/GPU pair instead of comparing incomplete score arrays.

## Limitations

- **Cleanup is fail-closed.** `vmaf_bench` retries a failed context close once.
  A second failure is reported on stderr and makes the run fail; imported GPU
  state stays owned until process exit.
- **Test data is pre-staged.** Nothing is downloaded or converted.
- **The resolution list is hard-coded** to `576x324`, `640x480`, `1280x720`,
  `1920x1080` and `3840x2160` (`core/tools/vmaf_bench.c`, the `resolutions`
  array). `--resolution WxH` restricts the run to one of them; new sizes need a
  source change.
- **No HIP or Metal path.** `meson` wires only the CUDA and SYCL dependencies
  into `vmaf_bench`.

## Related

- [cli.md](cli.md) — the scoring CLI (`vmaf`).
- [../benchmarks.md](../benchmarks.md) — canonical fork benchmark numbers.
- [../backends/index.md](../backends/index.md) — backend compile-time and
  runtime rules.
- `/cross-backend-diff` skill — wraps `vmaf_bench --validate` with PR-ready
  formatting.

## Former section names

Older pages and records link to these headings; each points to the section
that now holds its content.

### Performance benchmark (default)

Now under [Performance benchmark (default mode)](#performance-benchmark-default-mode).
