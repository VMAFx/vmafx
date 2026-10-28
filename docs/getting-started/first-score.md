# Score your first pair

This page runs one VMAF score on the clips that ship with the repository,
explains the output, and shows what to change for your own videos. It takes a
few minutes once you have a `vmaf` binary.

## Before you start

You need:

- a `vmaf` from [Getting started](index.md): a source build
  (`build/tools/vmaf`), the release binary, or the container image (Docker
  Engine 23.0 or later for images published after `v1.0.0-rc.2`,
  [details](../usage/docker.md#what-can-pull-the-images));
- a checkout of the repository, for the two test clips
  `testdata/ref_576x324_48f.yuv` (reference) and `testdata/dis_576x324_48f.yuv`
  (distorted): 48 frames each, 576x324 pixels, 8-bit, 4:2:0, raw YUV.

## 1. Run the score

From the repository root, with a source build:

```bash
build/tools/vmaf \
  --reference testdata/ref_576x324_48f.yuv \
  --distorted testdata/dis_576x324_48f.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --json --output scores.json
```

With the container image, mount the clips and let the CLI write the JSON to
standard output:

```bash
docker run --rm -v "$PWD/testdata:/data:ro" ghcr.io/vmafx/vmafx:v1.0.0-rc.2 \
  --reference /data/ref_576x324_48f.yuv \
  --distorted /data/dis_576x324_48f.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --json --output /dev/stdout > scores.json
```

A raw `.yuv` file has no header, so the four geometry options (`--width`,
`--height`, `--pixel_format`, `--bitdepth`) are required. On a terminal the CLI
shows a frame counter while it runs; `--quiet` turns it off.

On these two clips the run also prints two warnings:

```text
libvmaf WARNING est_params: covariance matrix singular, zeroing solution — further occurrences are counted and reported once at close
libvmaf WARNING est_params: covariance matrix was singular on 6 of 192 solves
```

!!! note
    The warnings come from the SpEED chroma feature of the default model, which
    sets the solution of a flat block to zero and keeps going. They report how
    often that happened; the run is not aborted. See
    [SpEED: singular covariance matrices](../metrics/speed.md#singular-covariance-matrices).

## 2. Read the result

`scores.json` has one entry per frame and a pooled summary. The pooled VMAF
score is the number most people want:

```bash
python3 -c "import json; print(json.load(open('scores.json'))['pooled_metrics']['vmaf'])"
```

```text
{'min': 94.591839, 'max': 100.0, 'mean': 95.938231, 'harmonic_mean': 95.909753}
```

VMAF runs from 0 to 100, where 100 means the distorted video cannot be told
apart from the reference. The last digits depend on the release you run.

The top-level keys of the file:

| Key | Contents |
| --- | --- |
| `version` | the VMAFx version that produced the file |
| `fps` | processing speed of the run |
| `frames` | one object per frame: `frameNum` and a `metrics` map with every feature value and the per-frame `vmaf` |
| `pooled_metrics` | `min`, `max`, `mean` and `harmonic_mean` of every metric over all frames |
| `aggregate_metrics` | summary values some features add; empty for the default model |
| `backend_used` | the backend that ran at least one feature extractor (`cpu`, `cuda`, `sycl`, `hip` or `metal`) |
| `feature_backends` | the backend of every extractor that ran, for example `{"extractor": "adm", "backend": "cpu"}` |

The long metric names, such as
`integer_adm2_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`,
are the features the model reads, with their options in the name. The
[feature reference](../metrics/features.md) explains each one.

Other output formats are one option away: `--xml` (the default when no format
is given), `--csv` and `--sub`. The [CLI reference](../usage/cli.md) describes
them.

## 3. Score your own videos

Three things change for real content.

1. **Containers and codecs.** The CLI reads raw `.yuv` and `.y4m` only.
   Convert compressed files to Y4M with FFmpeg, which keeps the geometry in the
   header so the four geometry options are no longer needed:

    ```bash
    ffmpeg -i reference.mp4 -pix_fmt yuv420p -f yuv4mpegpipe reference.y4m
    ffmpeg -i distorted.mp4 -pix_fmt yuv420p -f yuv4mpegpipe distorted.y4m
    vmaf --reference reference.y4m --distorted distorted.y4m --json --output scores.json
    ```

    For 10-bit content use `-pix_fmt yuv420p10le -strict -1`. To score without
    an intermediate file, use the [FFmpeg filter](../usage/ffmpeg.md).

2. **The model.** Without `--model`, the CLI uses `vmaf_v1.0.16_3d0h` (1080p at
   three picture heights). Upstream Netflix still defaults to `vmaf_v0.6.1`, so
   the same command gives different numbers there. Pick the model that matches
   your viewing condition, for example `--model version=vmaf_v1.0.16_1d5h_2160`
   for 4K; the [models overview](../models/overview.md) lists them all.

3. **Extra metrics.** Add other metrics to the same run with `--feature`, for
   example `--feature psnr` or `--feature cambi`. Each is listed in the
   [feature reference](../metrics/features.md).

## 4. Use a GPU

Add `--backend` with the backend your build or image includes:

```bash
vmaf --reference reference.y4m --distorted distorted.y4m \
     --backend cuda --json --output scores.json
```

An explicit backend never falls back silently. When it is not compiled in or
fails to start, the CLI stops with exit code `100`:

```text
vmaf: --backend cuda requested but this libvmaf was built without cuda support; refusing to silently fall back to CPU (ADR-0498)
```

`backend_used` and `feature_backends` in the JSON confirm what ran.
[Choose a backend](../backends/index.md) says which backend fits your hardware
and how close each one is to the CPU scores.

## Next steps

- [CLI reference](../usage/cli.md): every option, with examples.
- [FFmpeg](../usage/ffmpeg.md): score inside an FFmpeg pipeline.
- [VMAFx C API](../api/vmafx/index.md): embed VMAFx in an application
  (existing `libvmaf.h` callers: the [compatibility API](../api/index.md)).
- [Python library](../usage/python.md): the Python harness.
