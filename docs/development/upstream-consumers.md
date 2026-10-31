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

You need `git`, `meson`, `ninja`, `nasm`, `pkg-config`, `python3`, `zimg`
(`libzimg-dev`), the GStreamer 1.28 development packages with `gst-launch-1.0`
and the `rawvideoparse` plugin, and the glib development package (it provides
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
| `--prefix DIR` | libvmaf install under test. Required. Found through the `pkgconfig` directory of the prefix's `lib`, `lib64` or multiarch `lib/<triplet>` (Meson's default libdir on Debian and Ubuntu, for example `lib/x86_64-linux-gnu`, which the hosted runner uses), never through `-lvmaf` alone, so the split layout (`libvmaf.pc` requiring `libvmafx`) works unchanged. The same directories go on `LD_LIBRARY_PATH` for the scoring runs. |
| `--reference-prefix DIR` | Second install. The same consumer binary runs again with `LD_LIBRARY_PATH` pointing at it; scores must match. |
| `--against-cli` | Also score the same input with `DIR/bin/vmaf` (model `vmaf_v0.6.1`, default precision, one thread) and compare. |
| `--frames N` | Frames to score. Default 3. |
| `--work DIR` | Cache and scratch directory. Default `${TMPDIR:-/tmp}/upstream-consumers`. Source trees and builds are reused when the pin and the prefix are unchanged. |
| `--ref FILE`, `--dist FILE`, `--size WxH` | Raw yuv420p 8-bit inputs. Default: the repository's `src01_hrc00_576x324.yuv` and `src01_hrc01_576x324.yuv`. |
| `--planted-defect` | GStreamer script only, needs `--reference-prefix`: appends a digit to the first per-frame `self` score of the `--prefix` side of the first case and passes only when the comparison reports that difference (exit 0 with `PASS: the planted defect was detected`). It proves the comparison can fail. |
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
`format`, `split`, `trim` and `setpts` filters, the `rawvideo` demuxer and
decoder, the `null` muxer, the `file` protocol and `--enable-libvmaf`; only
`ffmpeg` is built. The distorted video is input 0 and the reference input 1,
which is the order of the filter's pads (`main`, then `reference`, in
`libavfilter/vf_libvmaf.c`). Each input is cut to `--frames` frames by `trim`
before the filter: stopping the output with `-frames:v` ends the run at a
scheduling-dependent point, and the last frame's motion score depends on whether
a following frame reached the filter.

With `--against-cli` the FFmpeg script also runs the filter graphs that
[SVT Encore](https://github.com/svt/encore) builds for its VMAF profile
options (`ff_encore_leg`), read from Encore's `CommandBuilder.kt` at the
pinned commit `8dd7c596c51a8c31bae805e31ff0969564f68229` (EUPL-1.2):
`sws_flags=bicubic`, the reference split in two, the distorted input scaled to
the reference (`scale=rw:rh`), then
`libvmaf=log_fmt=json:log_path=...` with `model`, `feature`, `n_threads` and
`n_subsample` appended in Encore's order and omitted when unset.
`uc_encore_graph` in `scripts/ci/upstream-consumer-lib.sh` writes the graph.
Five cases (defaults, two features, two models with Encore's `\\:` escape,
four threads with subsample 2, a reference filter) must give the CLI's JSON
exactly, and an unknown feature or model must stop FFmpeg with an error naming
it. Tests: `python3 -m pytest scripts/ci/tests/test_upstream_consumer_lib.py`.
When the pin moves, re-read `vmafParams` and the graph assembly in
`CommandBuilder.kt` and update `uc_encore_graph` with it.

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

The plugin is compiled once, against the headers of `--reference-prefix` when
one is given (else `--prefix`), and that one binary runs on both libraries.
With Netflix's libvmaf as the reference this is the pairing a distribution
ships: gst-plugins-bad built against the stock `libvmaf.h`, run on this fork's
`libvmaf.so.3`. The reverse pairing (a binary built against this fork's
headers, run on Netflix's library) is not supported: upstream's `VmafPicture`
has a trailing `color` member and is 16 bytes larger
([pictures](../api/pictures.md)).

The script scores every option of the upstream element, one case each:
`results-format` `json`, `xml` and `csv`; `psnr`, `ssim` and `ms-ssim` in each
format; `pool-method` `min`, `max`, `mean` and `harmonic_mean`; `subsample=2`;
`disable-clip`; `enable-transform`; `phone-model`; `conf-interval` (with the
bootstrap model `vmaf_b_v0.6.3`, which it requires); `threads=4`; and
`frame-message`. XML and CSV files are compared with the same exact-text
rules as JSON (`--format` of the comparator).

Every scoring run sets `LC_ALL=C`. Netflix's libvmaf prints numbers with the
locale's decimal separator, so under a locale such as `de_DE.UTF-8` its JSON
report holds `0,962084` and cannot be parsed; this fork's writers ignore the
locale.

**Comparison** (`scripts/ci/upstream_consumer_scores.py`). Reads the JSON that
all three tools write through `vmaf_write_output()`, keeps every number as the
literal text of the file, and compares every frame, every metric and the
`pooled_metrics` block. A missing frame, a missing metric or a difference in
the last printed digit (or a trailing zero) is a mismatch. Tests:
`python3 -m unittest discover -s scripts/ci/tests -p 'test_upstream_consumer_scores.py'`.

### CI

The `consumers` job of `.github/workflows/ffmpeg-integration.yml` (the
`Upstream Consumers` check) runs on changes under `core/`, to
`build-config.env`, to the scripts and to that workflow (the
`upstream_consumers` selector of `.github/ci-impact.json`), at the full CI tier
([ADR-2169](../adr/2169-ci-fewer-runs.md)): master pushes, dispatches, pull
requests from forks and pull requests labelled `ci: full`; an own pull request
at the light tier skips it. It builds the CPU
library at the pull request head and, on a pull request, at the base commit,
then runs both scripts with `--against-cli` and `--reference-prefix`. It also
builds Netflix's libvmaf at the upstream commit the fork records itself at
parity with (`scripts/ci/upstream_parity_pin.py`) and runs the GStreamer script
twice more against it: once with `--against-cli` (every case must be
identical on both libraries) and once with `--planted-defect` (the job fails
when the changed score goes unnoticed). Score files and logs are uploaded when
the job fails. The GPU leg (`--cuda`) needs a
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
