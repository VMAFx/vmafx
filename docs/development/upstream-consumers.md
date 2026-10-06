# Upstream consumer conformance

VMAFx keeps `libvmaf.so.3` and the `vmaf_*` C API as a thin compatibility
library on top of `libvmafx.so.1`, so software written for upstream libvmaf
keeps working without a patch. Two CI jobs prove it with the two most widely
used consumers: stock **FFmpeg** (its `libvmaf` filter) and stock
**GStreamer** (the `vmaf` element of gst-plugins-bad). Neither is patched.
Each one is built against the library under test, scores a few frames, and its
per-frame and pooled scores are compared with a second source as exact text.

A pass means: the consumer compiled against the installed headers and
`libvmaf.pc`, ran against the installed `libvmaf.so.3`, and produced scores
identical to the `vmaf` command-line tool and, on a pull request, identical
to the same consumer binary run against the library of the base commit.

## Run it locally

You need `git`, `meson`, `ninja`, `nasm`, `pkg-config`, `python3`, the
GStreamer 1.28 development packages with `gst-launch-1.0` and the
`rawvideoparse` plugin, and the glib development package (it provides
`glib-mkenums`). Network access is needed on the first run to fetch the two
source trees; they are cached in `--work`.

```bash
# 1. Install the library under test into a private prefix (CPU only).
meson setup build core --prefix "$PWD/_prefix" -Denable_dnn=disabled -Db_lto=false
ninja -C build -j4 && ninja -C build install

# 2. Unpatched upstream FFmpeg.
scripts/ci/upstream-ffmpeg-compat.sh --prefix "$PWD/_prefix" --against-cli

# 3. Unpatched upstream GStreamer vmaf plugin.
scripts/ci/upstream-gstreamer-compat.sh --prefix "$PWD/_prefix" --against-cli
```

To check a library change against the previous library, install the previous
commit into a second prefix and add `--reference-prefix <that prefix>`. The
same consumer binary is then run twice, once per library, and the two score
files must be identical.

Each script prints the consumer version, the exact `libvmaf` / `libvmafx`
files the dynamic loader executed (it fails when they are not under
`--prefix`), and one line `IDENTICAL` or `MISMATCH` per comparison. The last
line is `PASS` or `FAIL`.

## Reference

### Options

| Option | Meaning |
| --- | --- |
| `--prefix DIR` | libvmaf install under test. Required. Found through `lib/pkgconfig` and `lib64/pkgconfig` of the prefix, never through `-lvmaf` alone, so the split layout (`libvmaf.pc` requiring `libvmafx`) works unchanged. |
| `--reference-prefix DIR` | Second install. The same consumer binary runs again with `LD_LIBRARY_PATH` pointing at it; scores must match. |
| `--against-cli` | Also score the same input with `DIR/bin/vmaf` (model `vmaf_v0.6.1`, default precision, one thread) and compare. |
| `--frames N` | Frames to score. Default 3. |
| `--work DIR` | Cache and scratch directory. Default `${TMPDIR:-/tmp}/upstream-consumers`. Source trees and builds are reused when the pin and the prefix are unchanged. |
| `--ref FILE`, `--dist FILE`, `--size WxH` | Raw yuv420p 8-bit inputs. Default: the repository's `src01_hrc00_576x324.yuv` and `src01_hrc01_576x324.yuv`. |
| `--cuda` | FFmpeg script only: also run the upstream `libvmaf_cuda` filter. Prints `SKIP: <reason>` when the library has no CUDA, the host has no GPU runtime or `ffnvcodec` is missing; a skip is never reported as a pass. The GStreamer script prints a skip: the upstream element has no CUDA path. |

Environment: `UPSTREAM_CONSUMER_JOBS` (build parallelism, default 4) and
`UPSTREAM_CONSUMER_EXTRA_PKG_CONFIG_PATH` (extra pkg-config directories
searched after the prefix; useful when the distribution ships `glib-mkenums`
outside the path `glib-2.0.pc` names).

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Every comparison identical. |
| 1 | A comparison differs; the first difference is printed. |
| 2 | Setup failure: missing tool, fetch or build failure, wrong library loaded, unreadable score file. |

### What each script does

**FFmpeg** (`scripts/ci/upstream-ffmpeg-compat.sh`). Clones `FFMPEG_TAG` of
`FFMPEG_REMOTE` from `build-config.env` (shallow, no patch applied) and
configures a minimal build: `--disable-everything`, the `libvmaf`, `scale`,
`format`, `trim` and `setpts` filters, the `rawvideo` demuxer and decoder, the
`null` muxer, the `file` protocol and `--enable-libvmaf`; only `ffmpeg` is
built. The distorted video is input 0 and the reference input 1, which is the
order of the filter's pads (`main`, then `reference`, in
`libavfilter/vf_libvmaf.c`). Each input is cut to `--frames` frames by `trim`
before the filter: stopping the output with `-frames:v` ends the run at a
scheduling-dependent point, and the last frame's motion score depends on
whether a following frame reached the filter.

**GStreamer** (`scripts/ci/upstream-gstreamer-compat.sh`). Sparse-clones
`subprojects/gst-plugins-bad` of tag `GST_PLUGINS_BAD_VERSION` from the
GStreamer monorepo and builds only the `vmaf` plugin
(`-Dauto_features=disabled -Dvmaf=enabled`). The pipeline reads each file
with `filesrc` (one frame per buffer, `num-buffers=N`), `rawvideoparse` and
the element's `ref_sink` and `dist_sink` pads, with `results-format=json`.
The plugin path contains only the built plugin plus the system's core and raw
parser plugins, so the system's own `vmaf` plugin cannot run; the script
prints the `gst-inspect-1.0 vmaf` file name and fails when it is not the built
one. The element names its model `self`, so its score is called `self`; the
comparison maps that one name to `vmaf` (printed as a note) and still compares
every value as exact text.

**Comparison** (`scripts/ci/upstream_consumer_scores.py`). Reads the JSON that
all three tools write through `vmaf_write_output()`, keeps every number as the
literal text of the file, and compares every frame, every metric and the
`pooled_metrics` block. A missing frame, a missing metric or a difference in
the last printed digit (or a trailing zero) is a mismatch. Tests:
`python3 -m unittest discover -s scripts/ci/tests -p 'test_upstream_consumer_scores.py'`.

### CI

`.github/workflows/upstream-consumers.yml` runs on changes under `core/`, to
`build-config.env`, to the scripts and to the workflow. It builds the CPU
library at the pull request head and, on a pull request, at the base commit,
then runs both scripts with `--against-cli` and `--reference-prefix`. Score
files and logs are uploaded when the job fails. The GPU leg (`--cuda`) needs a
CUDA host and is not part of this job.

### How the pins move

- `FFMPEG_TAG` / `FFMPEG_REMOTE`: owned by the FFmpeg patch refresh
  ([FFmpeg patch automation](ffmpeg-patch-automation.md)); the same release is
  used here.
- `GST_PLUGINS_BAD_VERSION` / `GST_PLUGINS_BAD_REMOTE`: a plain git tag of the
  GStreamer monorepo, tracked by a Renovate regex manager in `renovate.json`
  that accepts stable releases only (even minor). A tag needs no archive
  digest, so Renovate can bump it alone. The script requires the installed
  `gstreamer-1.0` development files to have the same major.minor as the pin,
  so a minor bump stays red until the runner carries that GStreamer version.
