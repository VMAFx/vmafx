<!-- markdownlint-disable MD013 MD046 MD060 -->
# Using VMAF with FFmpeg

Compute VMAF inside an FFmpeg filter graph with the `vmafx` filter, the filter
of the VMAFx API (ADR-2166), or with stock upstream `libvmaf`.
This page covers the input-pad order, the first commands to run, the `vmafx`
filter and its options, the migration from the retired `libvmaf` filters, and
stock upstream `libvmaf` usage.

[FFmpeg](http://ffmpeg.org/) filters can score videos of different encodings
and resolutions directly. For the best practices of computing VMAF at the right
resolution, see
the Netflix [tech
blog](https://medium.com/netflix-techblog/vmaf-the-journey-continues-44b51ee9ed12).
Test videos are in
[vmaf_resource](https://github.com/Netflix/vmaf_resource/tree/master/python/test/resource).

> The pixel formats the library reads, and the lists the FFmpeg filter and the GStreamer element take, are generated from one table: see [Input pixel formats](pixel-formats.md).

## Input ordering (read this first) {#input-ordering}

!!! warning "FFmpeg takes the distorted video first and the reference second"
    The two input pads on FFmpeg filters (`vmafx`, `vmafx_tune`, as well as upstream `libvmaf`)
    are the opposite of the Python runner and the standalone `vmaf` CLI. FFmpeg pad 0 is
    the **distorted/main** stream and pad 1 is the **reference** stream. The Python and CLI
    surfaces take the reference first.

    Reversing the pads does not fail. It changes the direction of the
    comparison and can silently inflate the result: the measured Netflix-pair
    example in
    [Research-0730](../research/0730-ffmpeg-libvmaf-smoke-20260527.md#bug-2-input-ordering-trap-libvmaf-filter-reverses-refdis-vs-python-runner)
    produced `83.782079` in the wrong direction instead of the correct
    `76.667830`.

Correct: the distorted input feeds pad 0 and the reference input feeds pad 1.

```bash
ffmpeg -i distorted.mp4 -i reference.mp4 \
  -filter_complex "[0:v][1:v]vmafx=log_fmt=json:log_path=/dev/stdout" \
  -f null -
```

Incorrect: shown only so the dangerous inversion is recognizable. Do not use
this ordering.

<!-- vmafx-ffmpeg-input-order: intentionally-wrong -->
```bash
ffmpeg -i reference.mp4 -i distorted.mp4 \
  -filter_complex "[0:v][1:v]vmafx=log_fmt=json:log_path=/dev/stdout" \
  -f null -
```

The maintained FFmpeg patch also logs the pad contract at filter
initialization, but that message cannot repair an already reversed command.

## Install

Stock FFmpeg builds the upstream `libvmaf` filter once
[`libvmaf` is installed](../../core/README.md#install):

```bash
./configure --enable-libvmaf  # stock upstream FFmpeg
make -j4
make install
```

The fork's series builds the new `vmafx`, `vmafx_tune`, and `vmafx_pre` filters,
the `-vmafx-profile` and `-pass-autotune` options, and `-qpfile` support.
The series (`ffmpeg-patches/0001` through `0006`, listed in `ffmpeg-patches/series.txt`)
applies in order to a clean `n9.0.2` checkout after the shared FFmpeg fix series
([what it changes](../development/ffmpeg-patch-automation.md#shared-ffmpeg-fix-series)):

```bash
cd /path/to/ffmpeg && git checkout n9.0.2
/path/to/vmaf/scripts/ci/ffmpeg-shared-series.sh apply .
while IFS= read -r patch; do
    case "$patch" in ""|\#*) continue ;; esac
    git am --3way "/path/to/vmaf/ffmpeg-patches/$patch" || exit 1
done < /path/to/vmaf/ffmpeg-patches/series.txt
./configure --enable-libvmafx
make -j4
```

See [FFmpeg patch automation](../development/ffmpeg-patch-automation.md) for
how the series tracks the stable release. The patch series was redesigned and renumbered
from `0001` in issue #2435 (ADR-2166 superseding ADR-0860). All fork builds configure
`--enable-libvmafx` only.

To list the options of the locally installed filter, which is useful when this
page has drifted from the binary:

```bash
ffmpeg -h filter=vmafx
```

## First commands

### A pair of YUV files

Download the reference
[`src01_hrc00_576x324.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/src01_hrc00_576x324.yuv)
and the distorted
[`src01_hrc01_576x324.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/src01_hrc01_576x324.yuv)
video, then run:

```bash
ffmpeg -video_size 576x324 -r 24 -pixel_format yuv420p -i src01_hrc00_576x324.yuv \
    -video_size 576x324 -r 24 -pixel_format yuv420p -i src01_hrc01_576x324.yuv \
    -lavfi "[0:v]setpts=PTS-STARTPTS[reference]; \
            [1:v]setpts=PTS-STARTPTS[distorted]; \
            [distorted][reference]libvmaf=log_fmt=xml:log_path=/dev/stdout:model=version=vmaf_v0.6.1:n_threads=4" \
    -f null -
```

- `-r 24` sets the frame rate and must come before `-i`.
- `setpts=PTS-STARTPTS` synchronizes the presentation timestamps of the two
  videos. This is crucial if a video does not start at PTS 0, for example when
  it was cut from a long stream. FFmpeg filters synchronize on timestamps, not
  on frame counts.
- `log_path` is standard output (`/dev/stdout`).
- The model uses the preferred `model=version=` form. The older `model_path=`
  option is legacy and still accepted, but not recommended for new scripts.

The expected output (`vmaf_v0.6.1` on the Netflix pair, matching the `vmaf`
CLI's `76.66783025`):

```text
[libvmaf @ 0x7fcfa3403980] VMAF score: 76.667830
```

### Packaged `.mp4` inputs

This example takes a 480p reference
[`Seeking_30_480_1050.mp4`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/mp4/Seeking_30_480_1050.mp4)
and a 288p distorted video
[`Seeking_10_288_375.mp4`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/mp4/Seeking_10_288_375.mp4),
upsamples the distorted one to `720x480` with bicubic, and computes VMAF on the
two 480p videos. Bicubic is the recommended upsampling method; see the
[tech blog](https://medium.com/netflix-techblog/vmaf-the-journey-continues-44b51ee9ed12).

```bash
ffmpeg \
    -r 24 -i Seeking_30_480_1050.mp4 \
    -r 24 -i Seeking_10_288_375.mp4 \
    -lavfi "[0:v]setpts=PTS-STARTPTS[reference]; \
            [1:v]scale=720:480:flags=bicubic,setpts=PTS-STARTPTS[distorted]; \
            [distorted][reference]libvmaf=log_fmt=xml:log_path=/dev/stdout:model=version=vmaf_v0.6.1:n_threads=4" \
    -f null -
```

The expected output is:

```text
[libvmaf @ 0x7fb5b672bc00] VMAF score: 51.017497
```

### Score exactly N frames {#score-exactly-n-frames}

To score only the first N frames, trim **both inputs** to N frames:

```bash
ffmpeg -i dis.mp4 -i ref.mp4 \
    -lavfi "[0:v]trim=end_frame=200[d];[1:v]trim=end_frame=200[r];[d][r]vmafx=log_fmt=json:log_path=out.json" \
    -f null -
```

Do not cut the run with the output options `-frames:v N` or `-t`. FFmpeg
enforces them after the filtergraph, and the filter only learns that the output
has closed some time later. A VMAF scoring filter scores every pair it filters, so
it can score pairs that `ffmpeg` then drops: with `-frames:v 20` the Netflix
576x324 pair scored 21 frames in 7 of 12 runs with CPU scoring and 4 of 12 with
SYCL scoring, and with `-t 1` (24 frames) 25 in every run.
It is FFmpeg's behaviour (n9.0.2), the same for `vmafx` and upstream's `libvmaf`. `trim` on both inputs, or running to the end of
the input, gives exactly N in every run. `trim` also works on QSV frames, so the
zero-copy SYCL pipeline can be trimmed the same way.

See [FFmpeg's guide to libvmaf](https://ffmpeg.org/ffmpeg-filters.html#libvmaf),
the [FFmpeg Filtering Guide](https://trac.ffmpeg.org/wiki/FilteringGuide) for
more complex filters, and the
[Scaling Guide](https://trac.ffmpeg.org/wiki/Scaling) for scaling algorithms.

## Filters at a glance

| Filter / Option | Patch | Configure flag | Computes on |
| --- | --- | --- | --- |
| `vmafx` | `0004` | `--enable-libvmafx` | CPU, CUDA, SYCL, HIP, Metal: device-resident frames imported without a copy |
| `vmafx_tune` | `0004` | `--enable-libvmafx` | CPU / GPU; recommends a CRF for the next encode pass |
| `vmafx_pre` | `0005` | `--enable-libvmafx` | Learned pre-filter (ONNX) through the VMAFx DNN session |
| `-qpfile` | `0001` | `--enable-libvmafx` | Per-block QP offsets for libx264, libsvtav1, libaom-av1 |
| `-pass-autotune` | `0002` | `--enable-libvmafx` | Advisory CLI glue for external 2-pass autotuning |
| `-vmafx-profile` | `0003` | `--enable-libvmafx` | Advisory CLI glue for vmaf-tune profile hand-off |
| Loopback hwaccel | `0006` | `--enable-libvmafx` | GPU loopback decoder hwaccel preceding `-dec` |

Unpatched stock FFmpeg retains its own `libvmaf` filter (`--enable-libvmaf`) linking against the `libvmaf.so` compatibility wrapper.

Two `fftools` patches belong to the `vmafx` filters: `0003` adds
`-vmafx-profile`, and `0006` lets `-hwaccel`, `-hwaccel_device` and
`-hwaccel_output_format` precede `-dec`, so loopback decoders decode on the
GPU (see [Encode and score in one command](#encode-and-score-in-one-command)).

## The vmafx filter

`vmafx` is the FFmpeg filter of the VMAFx API (RC4,
[ADR-1852](../adr/1852-vmafx-api-redesign.md); the filters themselves are
[ADR-2125](../adr/2125-vmafx-ffmpeg-gstreamer-filters.md)). It scores a pair
of videos through `libvmafx` instead of `libvmaf`, follows the frames to the
device they live on, and reports through one unified set of options. The same engine
sits behind the native GStreamer element, which
[gstreamer.md](gstreamer.md) documents. See
[Migrating from the libvmaf filters](#migrating-from-the-libvmaf-filters)
for the mapping from old options to new spellings.

### Install the vmafx filter

The filter is patch `0004` of the series `ffmpeg-patches/0001` to `0006` in
`ffmpeg-patches/series.txt`; apply the whole series in order as in
[Install](#install). It builds against the installed `libvmafx` and needs
`pkg-config` to find `libvmafx >= 1.0.0`:

```bash
./configure --enable-libvmafx
make -j4
```

The sources of the fork filters and options are `ffmpeg-patches/src/vf_vmafx.c`,
`ffmpeg-patches/src/vf_vmafx_pre.c` and
`ffmpeg-patches/src/ffmpeg_dec_hwaccel.c`; patches `0004` to `0006` carry
them.

### Pads

The pads are in the order of `libvmaf`: pad 0 (`main`) is the distorted video
and pad 1 (`reference`) is the reference. The warning at the top of
[Input ordering](#input-ordering) applies unchanged. Reversing the pads does
not fail; it changes the direction of the comparison.

### Pairing the two inputs

The filter pairs a main frame with the reference frame of the same timestamp
(FFmpeg's framesync), and scores nothing it cannot pair. Three defaults
differ from the stock `libvmaf` filter, so that a score always means frame
against its own reference frame:

- `eof_action=pass` and `repeatlast=0`. A main frame with no reference frame (the
  reference starts later or ended earlier) passes on unscored, in order, and
  is counted: the log carries one warning at the first such frame and
  `vmafx: N main frames had no reference frame and passed unscored; the scores
  cover M pairs` at the end. The stock default (`eof_action=repeat`) scores the
  tail of a longer main stream against the last reference frame again, and a
  reference of 24 frames under a main of 48 gives a score that is wrong
  without a message. `eof_action=repeat:repeatlast=1` restores it; `repeat`
  alone has no effect, because `repeatlast=0` makes the framesync pass.
- Streams of different frame rate are paired by timestamp, not by frame
  number: the filter warns at configuration (`main runs at 24/1 fps and the
  reference at 30/1`) and counts pairs whose timestamps differ by more than
  half a frame (`vmafx: N of M pairs had main and reference timestamps more than
  half a frame apart`). Give both the same rate and start (`fps`, `setpts`,
  `trim`) before the filter.
- The init line `vmafx: input 0 (main) is the distorted video, input 1
  (reference) the reference` names the order; the pad names are the same.

Frames the timeline disabled (`enable=`) pass unscored without a warning. A run in
which no pair was scored (every frame disabled, or a reference that never
arrives) succeeds, writes no report (`log_path` is then not written, with a
warning) and logs no score. A `log_path` in a directory that does not exist
fails at configuration, not after the last frame.

To score the first N frames, cut each input (`trim=end_frame=N` before the
filter, or `-frames:v N` on each input): `-frames:v N` on the output stops the
graph when the encoder has N frames, and the filter may already have scored one
pair more.

### First vmafx command

With the Netflix pair from [A pair of YUV files](#a-pair-of-yuv-files):

```bash
ffmpeg -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 -i src01_hrc01_576x324.yuv \
    -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 -i src01_hrc00_576x324.yuv \
    -lavfi "[0:v][1:v]vmafx" -f null -
```

Here the first input is the distorted video and the second the reference. The
filter scores the library's default model (`vmaf_v1.0.16_3d0h`, reported as
`vmaf`) and logs, on a CPU-only host:

```text
vmafx provenance: {...}
vmafx vmaf mean: 82.816060
VMAF score: 82.816060
vmafx frames: 0 imported on the device, 96 host, 0 downloaded
```

The first line is the provenance record of the run (`provenance=log`, the
default). Each model and pool method then logs one `vmafx <model> <pool>:
<value>` line at the end, `VMAF score:` repeats the first model's mean, and the
last line says which path the frames took; both inputs count, so 48 frames of
each make 96. `model=version=vmaf_v0.6.1` gives `76.667831` on the same pair,
the value the `vmaf` CLI reports (`vmaf_v0.6.1neg` alone gives `75.073658`).
`ffmpeg-patches/test/vmafx_filter_check.py parity` checks the filter against
the CLI value by value ([Checking a build](#checking-a-vmafx-build)).

### Models and features {#how-feature-names-are-resolved-in-libvmaf_sycl}

`model` takes the spec of `vmafx_model_load_spec()` in
`core/include/vmafx/model.h`: colon-separated `key=value` items, with
`version=` (a built-in model) or `path=` (a model file), `name=` (the name
of its scores), `disable_clip`, `enable_transform` and
`<extractor>.<option>=<value>` overrides. A backslash escapes the next `:`,
`=`, `.` or backslash, which is why a colon inside a filtergraph is written
`\:`. Several models score in one pass when separated by `|`, and each needs
its own `name=`:

```bash
-lavfi "[0:v][1:v]vmafx=model='version=vmaf_v0.6.1\:name=vmaf|version=vmaf_v0.6.1neg\:name=vmaf_neg'"
```

logs `vmafx vmaf mean: 76.667831` and `vmafx vmaf_neg mean: 75.073658`. Two
models of one name are refused and the graph fails:

```text
version=vmaf_v0.6.1neg: VMAFX_E_INVALID: the context already scores a model of this name; give each one its own name (VmafxModelConfig.name, name= in a model spec) [vmaf]
```

`feature` registers extra extractors with the spec of
`vmafx_context_use_feature_spec()` (`core/include/vmafx/context.h`):
`<extractor>[=<key>=<value>[:...]]`, or upstream FFmpeg's
`name=<extractor>[:...]`, several joined by `|`. For example
`feature='psnr|cambi=full_ref=true'` with `log_path=out.json` adds `psnr_y`,
`psnr_cb`, `psnr_cr`, `cambi`, `cambi_full_reference` and `cambi_source` to the
report. The `integer_*` names map to their extractors as the CLI maps them.

An explicitly empty `model=` beside `feature` mounts no model, so the run
scores only the listed features, as the CLI's `--no_prediction` does:
`vmafx=model=:feature=psnr:log_path=out.json` writes the `psnr_*` metrics and
no `vmaf` score, and the filter prints no pooled-score or window lines. Without
`feature`, an empty or absent `model` scores the library's default model.

### Input colour tags and HDR models

A model file may declare a `conversion_target` (see
[model files](../models/v1.md#model-declared-conversion-target)). On the first
frame pair the `vmafx` filter reads the colour properties of each input (range,
primaries, transfer and matrix) and hands them to VMAFx with
`vmafx_context_set_default_color()` (`core/include/vmafx/context.h`). Tag
untagged files with `setparams` or `-color_primaries`, `-color_trc`,
`-colorspace` and `-color_range` on the input. An input whose four properties
are not all ones VMAFx knows (range `tv` or `pc`; primaries `bt709`, `bt2020`
or `smpte432`; transfer `bt709` or `smpte2084`; matrix `bt709`, `bt2020nc` or
`ictcp`) is left unspecified: a model without a `conversion_target`, which is
every shipped model, scores exactly as before, and a model with one refuses the
pair. The conversion is built from the first converted pair's colour, and a
later pair of another colour is refused. It uses `zimg` (enabled by default,
[ADR-3396](../adr/3396-zimg-on-by-default.md); opt-out builds with
`-Denable_zimg=false` return `-ENOTSUP`).

### Windows

`n_stats=<seconds>` or `n_stats_frames=<frames>` (exclusive with each other)
pools the scores over consecutive windows. With `pool=mean+min` and one-second
windows over the golden pair the log carries lines like:

```text
vmafx window: {"window":0,"start":0.000000000,"end":1.000000000,"first":0,"last":23,"n_frames":24,"n_scored":24,"partial":false,"v061":{"min":71.174759,"mean":76.326687}}
```

Window 1 covers frames 24 to 47 and is marked `"partial":true`, because the
stream ends inside it. `stats_out` chooses the destinations, flags joined by
`+`: `log` (the default), `metadata` and `file` (NDJSON, one object per window,
to `stats_path`). The windows equal the CLI's per-frame scores pooled offline
with `scripts/ci/vmafx_window_pooling.py`; the `windows` check compares them.
A window over a VMAF model completes one frame after its last frame, because
the motion features run incrementally
([ADR-2090](../adr/2090-motion-window-incremental.md)).

### Per-frame metadata

Window statistics reach the next output frame as
`lavfi.vmafx.window.<model>.<pool>` keys when `stats_out` has `metadata`.
`metadata=1` attaches every frame's scores as `lavfi.vmafx.<model>` and holds
the output until the frame's scores are final, which delays the output by the
retention depth of the models. Print them with `-vf metadata=print` after the
filter, or read them from the frames in a library caller.

### Hardware frames

`backend=auto` follows the frames. Software frames score on the CPU. CUDA
frames score on CUDA and are imported without a copy, ordered by CUDA event
fences. DRM PRIME frames (VAAPI frames mapped with `hwmap`) are imported as
dma-bufs without a copy on SYCL when the GPU is Intel's and on HIP when it is
AMD's; the device import waits on the dma-buf's implicit fences, so the
decoder's writes are complete before a kernel reads them, and a frame goes back
to its pool only after its release fence says the device read it. Vulkan
frames score on the device of their GPU (below). `backend=cuda` with
software frames makes the CUDA context upload them; the result equals
`vmaf --backend cuda` (`parity --backend cuda-host`).

`import` refines the rule: `auto` imports hardware frames and uploads software
frames, `device` refuses software frames, and `host` downloads hardware frames
and scores them on the CPU, logging `vmafx: import=host: hardware frames are
downloaded before scoring` once.

Other hardware frame types (VAAPI without `hwmap`, QSV, D3D11, D3D12,
VideoToolbox) pass format negotiation and are then refused by name:

```text
vmafx: backend auto cannot score vaapi frames; use import=host or hwdownload
```

The final `vmafx frames:` line counts how many frames were imported on the
device, scored on the host or downloaded.

VAAPI frames reach the filter as DRM PRIME through a DRM device the VAAPI
device is derived from (`hwmap=derive_device=drm` cannot derive one from a
plain VAAPI device):

```bash
ffmpeg -init_hw_device drm=dr:/dev/dri/renderD129 -init_hw_device vaapi=va@dr -filter_hw_device va \
  -hwaccel vaapi -hwaccel_device va -hwaccel_output_format vaapi -i distorted.mp4 \
  -hwaccel vaapi -hwaccel_device va -hwaccel_output_format vaapi -i reference.mp4 \
  -filter_complex "[0:v]hwmap=derive_device=drm,format=drm_prime[d]; \
                   [1:v]hwmap=derive_device=drm,format=drm_prime[r]; \
                   [d][r]vmafx" -f null -
```

The `format=drm_prime` after each `hwmap` matters: without it `hwmap` passes
the VAAPI frames on, which `vmafx` refuses by name.

QSV frames take one more step: FFmpeg maps a QSV frame only to its parent VAAPI
frame or to system memory, so a direct `hwmap=derive_device=drm` on QSV frames fails
with `Function not implemented`. Create the QSV device from a VAAPI device that is
itself derived from a DRM device (`-init_hw_device drm=dr:...`,
`-init_hw_device vaapi=va@dr`, `-init_hw_device qsv=qd@va`) and map each input with
`hwmap=derive_device=vaapi,format=vaapi,hwmap=derive_device=drm,format=drm_prime`.

#### Vulkan frames

Vulkan frames (FFmpeg's Vulkan decoder, `hwupload` to a Vulkan device, the
`libplacebo` filter) score on the VMAFx device on the same GPU: the filter
reads the Vulkan device's PCI location and opens the device of that location.
With `backend=auto` the GPU's vendor picks the backend (CUDA on NVIDIA, SYCL
on Intel, HIP on AMD); `backend=cuda`, `sycl` or `hip` picks it yourself. A
backend with no device on that GPU is refused by name: Vulkan frames are never
read across GPUs.

```bash
ffmpeg -init_hw_device vulkan=vk:NVIDIA -filter_hw_device vk \
  -hwaccel vulkan -hwaccel_device vk -hwaccel_output_format vulkan -i distorted.mp4 \
  -f rawvideo -pix_fmt yuv420p -s 1920x1080 -r 24 -i reference.yuv \
  -filter_complex "[1:v]format=nv12,hwupload[r];[0:v][r]vmafx" -f null -
```

The device name after `vk:` is matched against the Vulkan device names
(`vulkaninfo --summary` lists them); an index works too.

Each frame is copied once on the GPU before the handover. VMAFx imports one
exported image per plane, while FFmpeg's decoder writes one multi-plane image
whose memory is not exportable on every driver, and an FFmpeg frames context
does not say whether its pool exports its memory. The filter therefore copies
the planes, on the Vulkan device and behind the frames' timeline semaphores,
into its own pool of exportable images (one per plane; device-local OPTIMAL
images for CUDA, LINEAR images for the dma-buf paths of SYCL and HIP) and
hands those over. CUDA waits on the copy's timeline semaphores on its stream
and signals them again after the frame's last reader; SYCL and HIP take the
frame after a host wait for the copy and return it to the pool after its
release fence. No frame passes through host memory, and the `vmafx frames:`
line counts them as imported on the device. A planar Vulkan frame on CUDA is
read from CUDA arrays with one more device copy (`VMAFX_IMPORT_ALLOW_COPY`,
logged once by the library); NV12 and P010 are converted without one.

Planar and semi-planar layouts are taken; a packed layout (`yuyv422`, `y210`
and the like) is refused by name. The check is `vmafx_filter_check.py vulkan`
(below).

### Layouts

Besides planar YUV and NV12 / P010 / P016, the filter takes the 4:2:2 and
4:4:4 layouts the library imports
([ADR-2133](../adr/2133-vmafx-import-422-444-formats.md)), as software frames
and as device frames: NV16, P210, P216, NV24, P410, P416, Y210, Y212, YUYV422,
XV30, XV36, VUYX, and the MSB-aligned `yuv444p10msb` / `yuv444p12msb` in
which NVDEC returns 4:4:4 at 10 and 12 bits. Each scores as its planar
equivalent; the `layouts` check converts the golden pair to every one of them
and compares with the CLI on the planar file (all identical on the CPU and on
CUDA). A format whose samples are shifted, interleaved or packed in a way the
library has no layout for is refused by name, for example P212, the form
`hwupload` gives Y212 on CUDA: `vmafx: backend cuda: frames of layout p212le
cannot be scored`.

### Frame pools

A frame that `vmafx` has imported stays with the library until the library
releases it, and `metadata=1` holds the main frames until their scores are
final. With `-v verbose` the filter logs how many hardware frames it holds of
each input:

```text
vmafx: holds up to N hardware frames of each input (vmafx_context_max_in_flight() + 1)
```

`N = R + 2*T*(R+1) + 1`, plus 1 on a device backend, with `R` the frame
retention of the models and `T` the number of `threads`
(`vmafx_context_max_in_flight()` in `core/include/vmafx/context.h`). A filter
cannot enlarge the pool of the filter upstream of it, so a fixed-size pool
(VAAPI, QSV, D3D11, D3D12 and DXVA2 with an `initial_pool_size`) smaller than
`N`, where frames are held (device import, or the main input with
`metadata=1`), is refused before any frame:

```text
vmafx: the vaapi frame pool of the main input has 17 frames and vmafx holds up to 18 of them; give it at least 18 (-extra_hw_frames on the decoder, extra_hw_frames on hwupload)
```

`hwupload`'s pool has `2 + extra_hw_frames` frames. A decoder's pool also
holds the decoder's own reference frames, so give a decoder at least `N`
extra. Without the check a pool of 4 failed mid-stream with `ENOMEM` after
printing a partial score. Pools that grow (CUDA, Vulkan) are not limited. The
`pool` check of `vmafx_filter_check.py` measures this on VAAPI.

### Refusals

The filter never lets a frame that has a reference frame pass unscored. A frame the library cannot import
is retried once after a host wait on its acquire fence; then the graph fails
naming the backend, the input, the formats, the plane and each extractor that
refused it, and no score line follows. `feature=delta_e_itp` on CUDA frames is
the planted case: it names `delta_e_itp (cpu, ...)`, because that extractor
runs on the CPU only. A refusal at configuration (two models of one name, a
pool that is too small) logs nothing at uninit. This is decision D8 of
[ADR-1852](../adr/1852-vmafx-api-redesign.md); the `refusal` check proves
it.

### Encode and score in one command

The loopback decoder of FFmpeg 7.0 and later (`-dec`) feeds an encoder's output
back into a filter graph, so one command can encode, decode and score
([#2138](https://github.com/VMAFx/vmafx/issues/2138)). With NVIDIA hardware:

```bash
ffmpeg -init_hw_device cuda=cu:0 -filter_hw_device cu \
  -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 -i reference.yuv \
  -map 0:v -c:v h264_nvenc -preset p4 -rc constqp -qp 32 encoded.h264 \
  -hwaccel cuda -hwaccel_output_format cuda -dec 0:0 \
  -filter_complex "[0:v]hwupload[ref];[dec:0][ref]vmafx=n_stats=1:log_path=vmafx.json[out]" \
  -map "[out]" -f null -
```

Patch `0006` lets `-hwaccel`, `-hwaccel_device` and `-hwaccel_output_format`
precede `-dec`; stock FFmpeg reads them for input streams only, so without them
the loopback decoder returns system memory and the filter scores host frames.
The measured output on this command is two window lines (means `86.989539` and
`85.531429`), `vmafx vmaf mean: 86.260484`, `VMAF score: 86.260484` and
`vmafx frames: 96 imported on the device, 0 host, 0 downloaded`: the decoded
frames never touch the host. Patch `0006` is carried by the fork only
(decision Q-048) and is refreshed with every FFmpeg release
([ffmpeg-patch-automation.md](../development/ffmpeg-patch-automation.md)).
`vmafx_filter_check.py e2e` runs it.

### The vmafx_tune filter

`vmafx_tune` (patch `0004`) is the CRF-tuning filter of the VMAFx API: a
two-input filter that logs a recommended CRF for the next encode pass. Its
pads are `main` and `reference`.

```bash
ffmpeg -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 -i src01_hrc01_576x324.yuv \
    -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 -i src01_hrc00_576x324.yuv \
    -lavfi "[0:v][1:v]vmafx_tune=model=version=vmaf_v0.6.1" -f null -
```

logs `recommended_crf=27.2 (target_vmaf=95.0, observed_vmaf=76.67,
n_frames=48)`; `recommend_target_vmaf=80` gives `33.2`. Its options, from
`vmafx_tune_options` in `ffmpeg-patches/src/vf_vmafx.c`:

| Option | Default | Effect |
| --- | --- | --- |
| `model` | the library's default model | Model spec, as for `vmafx`. |
| `feature` | none | Extra extractors, `\|` separated. |
| `threads` | `0` | Worker threads (`0` to `1024`); `n_threads` is an alias. |
| `backend` | `auto` | Backend of software frames. |
| `device` | `auto` | GPU of the backend, `auto` or an index. |
| `recommend_target_vmaf` | `95.0` | Target VMAF score (0 to 100). |
| `recommend_crf_min` | `18.0` | Lower CRF bound (0 to 51). |
| `recommend_crf_max` | `51.0` | Upper CRF bound (0 to 51); must exceed the lower bound. |
| `recommend_passes` | `1` | Probe passes (1 to 8); advisory, one pass is scored. |

The mapping from the observed mean to a CRF is the piecewise linear curve
(`observed_to_crf()` in `vf_vmafx.c`); the per-clip search is
`tools/vmaf-tune`. The default model is `vmaf_v1.0.16_3d0h`. See
[Migrating](#migrating-from-the-libvmaf-filters).

### The vmafx_pre filter

`vmafx_pre` (patch `0005`) applies an ONNX learned pre-filter to a video
through the library's DNN session.

| Option | Default | Effect |
| --- | --- | --- |
| `model` | none, required | Path to the ONNX model. |
| `device` | `auto` | Inference device; the values of `tiny_device`. |
| `threads` | `0` | CPU execution provider intra-op threads. |
| `chroma` | `0` | `1` also filters U and V at their own size. |

8-bit planes run through `vmafx_dnn_session_run_luma8` and 10 and 12-bit
planes through `vmafx_dnn_session_run_plane16`. A failed inference fails the
frame and names the plane.

### The -vmafx-profile option

Patch `0003` adds `-vmafx-profile <path>` to `fftools`. It logs the profile path
and the `vmaf-tune` command that reads the report and encodes with it.

### Limits

- Dolby Vision profile 5 sources need converting outside the pipeline before
  scoring; VMAFx reads and applies no RPU data
  ([ADR-1685](../adr/1685-post-1-0-embedding-zero-copy-milestone.md),
  decision Q-047).
- `target_width`, `target_height` and `target_scaling` are reserved until RC5
  (device-targeted scoring) and the filter refuses them when set.
- The filter imports CUDA frames, DRM PRIME frames (on SYCL and HIP) and
  Vulkan frames (on the device of their GPU). QSV and VideoToolbox frames are
  refused by name until the filter's import slots for them are filled;
  `import=host` or `hwdownload` scores them on the CPU.
- FFmpeg 9.0.2's `libplacebo` filter fails to initialise ("Error initializing
  filters", no message) with libplacebo 7.360 or older on a Vulkan device
  that has internally synchronized queues (NVIDIA 615, Mesa 26.2): FFmpeg
  sets queue flags that only libplacebo API 365 accepts, which no libplacebo
  release has yet. This is outside VMAFx; the `libplacebo` route was checked
  with libplacebo built from its development branch on NVIDIA and Intel, and
  with libplacebo 7.360.0 on Mesa 26.0.8 (no such queues) on AMD
  (`T-FFMPEG-LIBPLACEBO-QUEUE-FLAGS-2026-10-07` in [state.md](../state.md)).
- `libvmafx` creates devices of the backends it was built with: the CPU, and
  CUDA, SYCL or HIP. `backend=metal`, or a backend the library was built
  without, fails at configuration and names the backend.
- The two inputs must match in size and in scored layout. Every layout scores
  as its planar equivalent (`scored_layout()` in
  `ffmpeg-patches/src/vf_vmafx.c`), so a decoder's NV12 frames and an uploaded
  YUV420P reference score together; other mixes are refused.
- NVENC on an RTX 4090 encodes 4:4:4 at 10 bits only, so the 12-bit 4:4:4
  decode path is checked through uploads (`layouts`), not through an encode.
- AMD VAAPI surfaces are tiled, and the HIP device reads linear dma-bufs only:
  on an AMD GPU the DRM PRIME import is refused by name (naming the plane's
  `modifier`). Give `vmafx` the VAAPI frames themselves (no `hwmap`) with
  `import=host`: the VAAPI download de-tiles them (golden pair: `76.667831`,
  the CLI's value). FFmpeg refuses to download a tiled DRM PRIME mapping
  (`T-HIP-VAAPI-TILED-SURFACES-2026-10-07` in [state.md](../state.md)).
- `vmafx_pre` refuses the shipped `learned_filter_v1` model: its ONNX input
  is `[batch,1,224,224]` and the pre-filter path takes a static `[1,1,H,W]`
  model (state row `T-VMAFX-PRE-LEARNED-FILTER-V1-REFUSED-2026-10-06` in
  [state.md](../state.md)).

### Checking a vmafx build

`ffmpeg-patches/test/vmafx_filter_check.py` compares the filter with the `vmaf`
CLI of the same build. Every subcommand takes `--ffmpeg`, `--vmaf` and
`--libdir` and exits 0 on success, 1 on a failure and 77 when a fixture is
missing.

CI builds FFmpeg with the patch series and `--enable-libvmafx` on every pull
request that touches the C core, the patches or the workflows, and runs
`filters`, then `parity`, `windows`, `contract` and `provenance` on the Netflix
576x324 pair against the CLI of the same build: the required check `FFmpeg vmafx Filter` in
`.github/workflows/ffmpeg-patch-stack.yml`. A missing fixture (exit 77) fails
that job. The GPU, VAAPI and Vulkan subcommands need a device and run on a
workstation only.

| Subcommand | Checks | Needs |
| --- | --- | --- |
| `filters` | The build registers `vmafx` and no filter of the stock upstream libvmaf family: no name that starts with `vmaf` or `libvmaf` other than `vmafx`, `vmafx_tune` and `vmafx_pre`. FFmpeg's own `vmafmotion` filter, which uses no library, is allowed. `ffmpeg-patches/test/test_vmafx_filter_names.py` holds its positive and negative cases. | CPU |
| `parity` | Every per-frame, pooled and aggregate value of the filter's JSON report equals the CLI's, as the same double. `--backend cuda` and `cuda-host` compare against `vmaf --backend cuda`. | CPU; CUDA for the two GPU forms |
| `windows` | `stats_path` windows equal the CLI's per-frame scores pooled offline. | CPU |
| `provenance` | The report carries the provenance record and the log its init line. | CPU |
| `refusal` | A CPU-only extractor on CUDA frames fails the graph naming the backend, input and extractor, and no score line follows. | CUDA |
| `pool` | Both inputs in fixed VAAPI pools: a pool one frame short is refused by name, a pool of exactly `N` frames scores as the CLI. | VAAPI device |
| `contract` | The pairing and error contract on host frames: a short reference leaves main frames unscored and counted and the score equals the CLI's over the paired frames, unequal rates are warned about, a timeline-disabled filter is not an error, an unwritable `log_path` fails at init, the init line names the input order. | CPU |
| `layouts` | Every 4:2:2 / 4:4:4 layout scores as the CLI scores the planar file it was converted from; `--backend cuda` imports them on the device. | CPU; CUDA for the device form |
| `vulkan` | The reference encoded on the `--vendor`'s GPU, decoded by FFmpeg's Vulkan decoder on the same GPU and scored by `vmafx` against the reference uploaded to the Vulkan device: every value equals the CLI's on the downloaded decoded frames, and every frame of both inputs is imported on the device. `--libplacebo` puts the `libplacebo` filter between decoder and `vmafx`. | `--enable-vulkan` and the vendor's Vulkan H.264 decode (`ANV_DEBUG=video-decode` on Intel with Mesa 26); `--enable-libplacebo` for `--libplacebo` |
| `e2e` | The encode-and-score command above equals the same frames scored from files by the CLI on the same backend, windows included, with every frame imported on the device. `--vendor nvidia` (NVENC, NVDEC, CUDA; `--e2e-format 444p10` for HEVC 4:4:4 10-bit), `--vendor intel` (VAAPI encode and decode, DRM PRIME, SYCL) or `--vendor amd` (the same on HIP) with `--render-node`. | The vendor's encoder and decoder, `--enable-libvmafx` (with `--enable-vaapi --enable-libdrm` for Intel and AMD) |

## `vmafx` filter option table

The options of the `vmafx` filter are generated from the option groups of
`core/api/vmafx.toml` ([ADR-1852](../adr/1852-vmafx-api-redesign.md)) into
`ffmpeg-patches/src/vf_vmafx_options.h`, the table the filter compiles; the
table below is that generated text. Upstream option names (`n_threads`,
`n_subsample`) stay accepted as aliases. [The sections above](#the-vmafx-filter)
explain how the options combine.

<!-- BEGIN GENERATED: vmafx-api vmafx filter options (scripts/codegen/vmafx-api.py) -->

| Option | Aliases | Value | Default | Description |
| --- | --- | --- | --- | --- |
| `model` | | string | library default (`VMAF_DEFAULT_MODEL_VERSION`) | Model, colon-delimited: version= a built-in model, path= a model file, name= the name in the report, disable_clip, enable_transform, &lt;feature&gt;.&lt;option&gt;=&lt;value&gt; overloads. Several models score in one pass (repeat the option; the filter separates them with \|). |
| `backend` | | `auto` \| `cpu` \| `cuda` \| `sycl` \| `hip` \| `metal` | `auto` | Backend: auto uses the available ones; any other value runs that backend alone and fails when it is not available. |
| `feature` | | string | | Additional feature extractor, name[=key=value:...] (for example psnr or cambi=full_ref=true); several may be given (the filter separates them with \|). Mutually exclusive with the CTC presets. |
| `tiny_model` | | string | | Tiny ONNX model to load alongside the classic models. |
| `tiny_device` | | `auto` \| `cpu` \| `cuda` \| `openvino` \| `openvino-npu` \| `openvino-cpu` \| `openvino-gpu` \| `coreml` \| `coreml-ane` \| `coreml-gpu` \| `coreml-cpu` \| `rocm` | `auto` | ONNX Runtime execution provider of the tiny model. |
| `tiny_threads` | | uint | | Intra-op threads of the CPU execution provider (0: the runtime's default). |
| `tiny_fp16` | | bool | `false` | Request fp16 input and output where the execution provider supports it. |
| `threads` | `n_threads` | uint | | Worker threads of the feature extractors, capped to the hardware threads (0: score in the calling thread). |
| `cpumask` | | uint | | Bitmask of CPU instruction sets the extractors must not use. |
| `gpumask` | | uint | | Bitmask of GPU operations the extractors must not use. |
| `device` | | string | `auto` | GPU of the selected backend: auto, or a device index. Needs a GPU backend that selects devices by index (sycl, hip, metal). |
| `subsample` | `n_subsample` | uint >= 1 | `1` | Score every n-th frame (1: every frame). |
| `log_path` | | string | | Report file. |
| `log_fmt` | | `json` \| `xml` \| `csv` \| `sub` | `json` | Report format; json and xml carry the backend receipt. |
| `view_distance` | | float 3..24 | | Viewing distance in display heights (ADM adm_norm_view_dist). Unset: the model's value. The ADM extractor's default CSF refuses a distance below 3 (it accepts 0.75 with other CSF modes, which these options do not set). |
| `display_height` | | uint >= 1 | | Height of the reference display in pixels (ADM adm_ref_display_height). Unset: the model's value. |
| `target_width` | | uint | `0` | Width of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_height` | | uint | `0` | Height of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_scaling` | | `none` \| `bilinear` \| `bicubic` \| `lanczos` | `none` | Scaling filter towards the target display. Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `pool` | | `min` \| `max` \| `mean` \| `harmonic_mean` \| `median` \| `perc5` \| `perc10` \| `perc20` | `mean` | Pool methods of the final score and of the windows. |
| `n_stats` | | float >= 0 | `0.0` | Window length in seconds (0: no windows). |
| `n_stats_frames` | | uint | `0` | Window length in frames (0: no windows); exclusive with n_stats. |
| `stats_out` | | `log` \| `metadata` \| `file` | `log` | Where window statistics go. |
| `stats_path` | | string | | NDJSON file of the window statistics, one object per window. |
| `score_fmt` | | string | `%.6f` | printf format of the scores (%.17g is lossless). |
| `provenance` | | `log` \| `report` | `log+report` | Print the provenance record at init (log) and embed it in the report (report). |
| `metadata` | | bool | `false` | Attach per-frame scores to the frames as lavfi.vmafx.&lt;metric&gt; metadata (delays output by the retention depth). |
| `profile` | | bool | `false` | Log the device timing breakdown at uninit. |
| `import` | | `auto` \| `device` \| `host` | `auto` | auto imports hardware frames without a copy and uploads software frames; device refuses software frames; host downloads hardware frames (logged once with the reason). |
| `perceptual_weight` | | bool | `false` | Weight the scores with the perceptual side data of the frames. |

<!-- END GENERATED: vmafx-api vmafx filter options -->

## Migrating from the libvmaf filters

The fork's FFmpeg patch series was redesigned in issue #2435: all standalone
backend filters (`libvmaf_cuda`, `libvmaf_sycl`, `libvmaf_metal`), tuning and
pre-filter names, and changes to `vf_libvmaf.c` were eliminated and unified into
the `vmafx` filter family (`vmafx`, `vmafx_tune`, `vmafx_pre`, `-vmafx-profile`).
Fork builds configure with `--enable-libvmafx`. Unpatched upstream FFmpeg retains
its own `libvmaf` filter against the backwards-compatibility library (`libvmaf.so.3`).

### Surface mapping table

| Old Filter / Location | Old Option / Surface | New Filter / Location | New Option / Surface | Status / Mapping Notes |
| --- | --- | --- | --- | --- |
| `libvmaf` | `model` | `vmafx` | `model` | Ported (`model=version=...` or `model=path=...`). |
| `libvmaf` | `log_path` | `vmafx` | `log_path` | Ported. |
| `libvmaf` | `log_fmt` | `vmafx` | `log_fmt` | Ported (`json`, `xml`, `csv`, `sub`). |
| `libvmaf` | `pool` | `vmafx` | `pool` | Ported (`min`, `max`, `mean`, `harmonic_mean`, `median`, `perc5`, `perc10`, `perc20`). |
| `libvmaf` | `n_threads` | `vmafx` | `threads` / `n_threads` | Ported (`n_threads` kept as alias). |
| `libvmaf` | `n_subsample` | `vmafx` | `subsample` / `n_subsample` | Ported (`n_subsample` kept as alias). |
| `libvmaf` | `feature` | `vmafx` | `feature` | Ported (`feature=name=...` or `feature=psnr\|float_ssim`). |
| `libvmaf` | `cuda` (`cuda=1`) | `vmafx` | `backend=cuda` | Ported. |
| `libvmaf` | `sycl_device` | `vmafx` | `backend=sycl:device=N` | Ported. |
| `libvmaf` | `sycl_profile` | `vmafx` | `profile=1` | Ported. |
| `libvmaf` | `vulkan_device` | — | — | **DROPPED** (no new spelling; ADR-0726 / ADR-0860 / ADR-2166). |
| `libvmaf` | `hip_device` | `vmafx` | `backend=hip:device=N` | Ported. |
| `libvmaf` | `metal_device` | `vmafx` | `backend=metal:device=N` | Ported. |
| `libvmaf` | `tiny_model` | `vmafx` | `tiny_model` | Ported. |
| `libvmaf` | `tiny_device` | `vmafx` | `tiny_device` | Ported (`auto`, `cpu`, `cuda`, `openvino`, `coreml`, `rocm`, etc.). |
| `libvmaf` | `tiny_threads` | `vmafx` | `tiny_threads` | Ported. |
| `libvmaf` | `tiny_fp16` | `vmafx` | `tiny_fp16` | Ported. |
| `libvmaf` | `perceptual_weight` | `vmafx` | `perceptual_weight` | Ported (reads Pelorus frame side-data). |
| `libvmaf` | `enable_conf_interval` | — | — | **DROPPED** (upstream 1.x legacy feature; not in `vmafx`). |
| `libvmaf_sycl` | (whole filter) | `vmafx` | `backend=sycl` `import=auto\|device` | Ported. Zero-copy QSV/VAAPI unified in `vmafx`. |
| `libvmaf_sycl` | `gpu_profile` | `vmafx` | `profile=1` | Ported. |
| `libvmaf_vulkan` | (whole filter) | — | — | **DROPPED** (no new spelling; ADR-0726 / ADR-0860 / ADR-2166). |
| `libvmaf_metal` | (whole filter) | `vmafx` | `backend=metal` `import=host` | Ported. VideoToolbox frames downloaded via `import=host` (zero-copy IOSurface import not implemented). |
| `libvmaf_tune` | (whole filter) | `vmafx_tune` | (filter name mapping) | Ported. Recommends CRF for next encode pass. |
| `libvmaf_tune` | `model`, `feature`, `n_threads` | `vmafx_tune` | `model`, `feature`, `threads`/`n_threads` | Ported. |
| `libvmaf_tune` | `recommend_target_vmaf` | `vmafx_tune` | `recommend_target_vmaf` | Ported (default 95.0). |
| `libvmaf_tune` | `recommend_crf_min` | `vmafx_tune` | `recommend_crf_min` | Ported (default 18.0). |
| `libvmaf_tune` | `recommend_crf_max` | `vmafx_tune` | `recommend_crf_max` | Ported (default 51.0). |
| `libvmaf_tune` | `recommend_passes` | `vmafx_tune` | `recommend_passes` | Ported (default 1). |
| `libvmaf_tune` | (none) | `vmafx_tune` | `backend`, `device` | Added: device execution for tuning pass. |
| `vmaf_pre` | (whole filter) | `vmafx_pre` | (filter name mapping) | Ported (`vf_vmafx_pre.c`). |
| `vmaf_pre` | `model`, `device`, `threads`, `chroma` | `vmafx_pre` | `model`, `device`, `threads`, `chroma` | Ported. |
| `-vmaf-profile` | `-vmaf-profile` | ffmpeg CLI | `-vmafx-profile` | Ported (`fftools/ffmpeg_opt.c`). |
| `-pass-autotune` | `-pass-autotune` | ffmpeg CLI | `-pass-autotune` | Kept (`fftools/ffmpeg_opt.c`). |
| Retryable close | context close retry | `vmafx` | `release_all()`, `tune_uninit()` | Ported. |
| Mid-run error pool | suppress pooled score | `vmafx` | `s->failed = 1` guard in `uninit()` | Ported. |
| Colorimetry | `vmaf_set_input_colorimetry` | `vmafx` | AVFrame colorimetry mapping | Ported (range, primaries, trc, matrix). |

### Dropped options and filters

- **`libvmaf_vulkan` and `vulkan_device`**: Completely dropped without replacement
  (ADR-0726, ADR-0860, ADR-2166). The Vulkan backend in libvmaf was retired.
  Hardware decode via `-hwaccel vulkan` remains available in FFmpeg, and frames can
  be downloaded or imported to other devices for scoring by `vmafx`.
- **`enable_conf_interval`**: Dropped. This was a legacy upstream 1.x confidence
  interval option that is not supported by the VMAFx engine.

### Default model change

- Upstream `libvmaf` and old fork filters defaulted to `vmaf_v0.6.1`.
- `vmafx` and `vmafx_tune` default to the modern library default `vmaf_v1.0.16_3d0h`.
- Scores of the default model differ from `vmaf_v0.6.1` by design (e.g. `82.816060` vs `76.667831`
  on the Netflix 576x324 reference pair).
- To preserve exact legacy scores, explicitly specify `model=version=vmaf_v0.6.1`.

### Worked migration examples

#### 1. Basic CPU scoring

Old command:

```bash
ffmpeg -i degraded.mp4 -i reference.mp4 \
  -lavfi "[0:v][1:v]libvmaf=model=version=vmaf_v0.6.1:log_path=scores.json:log_fmt=json:n_threads=4" \
  -f null -
```

New command:

```bash
ffmpeg -i degraded.mp4 -i reference.mp4 \
  -lavfi "[0:v][1:v]vmafx=model=version=vmaf_v0.6.1:log_path=scores.json:log_fmt=json:threads=4" \
  -f null -
```

#### 2. CUDA GPU scoring

Old command (`libvmaf_cuda` filter):

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i degraded.mp4 \
  -hwaccel cuda -hwaccel_output_format cuda -i reference.mp4 \
  -filter_complex "[0:v]scale_cuda=format=yuv420p[d];[1:v]scale_cuda=format=yuv420p[r];[d][r]libvmaf_cuda=log_fmt=json:log_path=scores.json" \
  -f null -
```

New command (`vmafx` with `backend=cuda`; imports CUDA frames directly without scale copy):

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i degraded.mp4 \
  -hwaccel cuda -hwaccel_output_format cuda -i reference.mp4 \
  -filter_complex "[0:v][1:v]vmafx=backend=cuda:log_fmt=json:log_path=scores.json" \
  -f null -
```

#### 3. SYCL GPU scoring (QSV zero-copy)

Old command (`libvmaf_sycl` filter):

```bash
ffmpeg -hwaccel qsv -hwaccel_output_format qsv -i degraded.mp4 \
  -hwaccel qsv -hwaccel_output_format qsv -i reference.mp4 \
  -filter_complex "[0:v][1:v]libvmaf_sycl=log_fmt=json:log_path=scores.json" \
  -f null -
```

New command (`vmafx` with `backend=sycl`):

```bash
ffmpeg -init_hw_device drm=dr:/dev/dri/renderD128 -init_hw_device vaapi=va@dr \
  -init_hw_device qsv=qd@va -init_hw_device qsv=qr@va \
  -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qd -i degraded.mp4 \
  -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qr -i reference.mp4 \
  -filter_complex "[0:v]hwmap=derive_device=vaapi,format=vaapi,hwmap=derive_device=drm,format=drm_prime[d]; \
                   [1:v]hwmap=derive_device=vaapi,format=vaapi,hwmap=derive_device=drm,format=drm_prime[r]; \
                   [d][r]vmafx=backend=sycl:log_fmt=json:log_path=scores.json" \
  -f null -
```

#### 4. CRF recommendation tuning

Old command (`libvmaf_tune` filter):

```bash
ffmpeg -i degraded.mp4 -i reference.mp4 \
  -lavfi "[0:v][1:v]libvmaf_tune=recommend_target_vmaf=95:model=version=vmaf_v0.6.1" \
  -f null -
```

New command (`vmafx_tune` filter):

```bash
ffmpeg -i degraded.mp4 -i reference.mp4 \
  -lavfi "[0:v][1:v]vmafx_tune=recommend_target_vmaf=95:model=version=vmaf_v0.6.1" \
  -f null -
```

#### 5. Learned pre-filter

Old command (`vmaf_pre` filter):

```bash
ffmpeg -i input.mp4 \
  -vf "vmaf_pre=model=model/tiny/learned_filter_v1.onnx:device=cuda" \
  output.mp4
```

New command (`vmafx_pre` filter):

```bash
ffmpeg -i input.mp4 \
  -vf "vmafx_pre=model=model/tiny/learned_filter_v1.onnx:device=cuda" \
  output.mp4
```

#### 6. Encoder profile generation

Old command (`-vmaf-profile` option):

```bash
ffmpeg -i input.mp4 -c:v libsvtav1 -vmaf-profile sweep_profile.html -f null -
```

New command (`-vmafx-profile` option):

```bash
ffmpeg -i input.mp4 -c:v libsvtav1 -vmafx-profile sweep_profile.html -f null -
```

## Upstream `libvmaf` filter (unpatched FFmpeg)

Unpatched stock FFmpeg uses its built-in `libvmaf` filter (source:
[`libavfilter/vf_libvmaf.c`](https://github.com/FFmpeg/FFmpeg/blob/master/libavfilter/vf_libvmaf.c))
configured with `--enable-libvmaf`, linking against the `libvmaf.so.3` backwards-compatibility
library that VMAFx builds.

In unpatched upstream FFmpeg, options follow the filter name inside an `-lavfi` or `-filter_complex` expression:

```text
[0:v][1:v]libvmaf=model=version=vmaf_v0.6.1:log_path=/dev/stdout:log_fmt=json:n_threads=4
```

Upstream stock options include: `model`, `log_path`, `log_fmt`, `feature`, `pool`, `n_threads`, and `n_subsample`.

All fork-added options and standalone backend filters have been retired in issue #2435. For modern VMAFx features, GPU execution, learned pre-filtering, and encoder tuning, build FFmpeg with `--enable-libvmafx` and use the unified `vmafx`, `vmafx_tune`, and `vmafx_pre` filters. See [Migrating from the libvmaf filters](#migrating-from-the-libvmaf-filters) for the full migration guide.

## GPU-accelerated scoring with vmafx

The `vmafx` filter unifies CPU and GPU compute under a single filter name and option set. Specify the compute backend with `backend=` (`cpu`, `cuda`, `sycl`, `hip`, `metal`).

### Per-backend copy-paste examples

All examples operate on the same input pair (`distorted.mp4`, `reference.mp4`).

**CPU** (default; computes on host):

```bash
ffmpeg -i distorted.mp4 -i reference.mp4 \
       -filter_complex "[0:v][1:v]vmafx=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**CUDA** (NVIDIA GPU; libvmafx built with `-Denable_cuda=true`):

Software-decoded frames uploaded to CUDA:

```bash
ffmpeg -i distorted.mp4 -i reference.mp4 \
       -filter_complex "[0:v][1:v]vmafx=backend=cuda:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

Device-resident frames decoded with NVDEC (`scale_cuda` to `yuv420p`):

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i distorted.mp4 \
       -hwaccel cuda -hwaccel_output_format cuda -i reference.mp4 \
       -filter_complex "[0:v]scale_cuda=format=yuv420p[d];[1:v]scale_cuda=format=yuv420p[r];[d][r]vmafx=backend=cuda:import=auto:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**SYCL** (Intel Arc / Data Center GPU; libvmafx built with `-Denable_sycl=true`):

Software-decoded frames uploaded to SYCL device 0:

```bash
ffmpeg -i distorted.mp4 -i reference.mp4 \
       -filter_complex "[0:v][1:v]vmafx=backend=sycl:device=0:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

QSV / oneVPL zero-copy hardware frames:

```bash
ffmpeg -init_hw_device drm=dr:/dev/dri/renderD128 -init_hw_device vaapi=va@dr \
       -init_hw_device qsv=qd@va -init_hw_device qsv=qr@va \
       -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qd -i distorted.mp4 \
       -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qr -i reference.mp4 \
       -filter_complex "[0:v]hwmap=derive_device=vaapi,format=vaapi,hwmap=derive_device=drm,format=drm_prime[d]; \
                        [1:v]hwmap=derive_device=vaapi,format=vaapi,hwmap=derive_device=drm,format=drm_prime[r]; \
                        [d][r]vmafx=backend=sycl:import=auto:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**Metal** (Apple Silicon; libvmafx built with `-Denable_metal=enabled` or `auto`):

VideoToolbox hardware frames downloaded to host for Metal scoring:

```bash
ffmpeg -hwaccel videotoolbox -hwaccel_output_format videotoolbox_vld -i distorted.mp4 \
       -hwaccel videotoolbox -hwaccel_output_format videotoolbox_vld -i reference.mp4 \
       -filter_complex "[0:v][1:v]vmafx=backend=metal:import=host:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

## Model path on Windows

Built-in models need no path: use `model=version=vmaf_v0.6.1`, and skip this
section. It applies only when you pass a file path for a model file.

A relative `path` inside `model='path=...'` needs no escaping. An absolute Windows path such as
`D:\mypath\vmaf_v0.6.1.json` must be escaped so that `ffmpeg` hands the right
path to `libvmafx`. The final command line depends on the shell.

1. Convert every backslash to a forward slash: `D:/mypath/vmaf_v0.6.1.json`.
2. Escape the colon with a backslash: `D\:/mypath/vmaf_v0.6.1.json`.
3. Escape that backslash with another backslash: `D\\:/mypath/vmaf_v0.6.1.json`.
4. Pick the form for your shell.

**PowerShell and Command Prompt.** The path from step 3 is enough:

```powershell
./ffmpeg.exe -i dist.y4m -i ref.y4m \
    -filter_complex "[0:v][1:v]vmafx=model='path=D\\:/mypath/vmaf_v0.6.1.json'" \
    -f null -
```

Neither shell treats `\` as special, so the quotes around the path are
optional here, and quoting the whole text after `-filter_complex` gives the same result.

**bash (msys2).** Two behaviours matter:

- bash treats an unquoted `\` as an escape character, so the escaped backslash
  needs another backslash.
- msys2 converts POSIX-like arguments such as
  `/mingw64/share/model/vmaf_v0.6.1.json` to Windows mixed paths
  (`D:/msys2/mingw64/share/model/vmaf_v0.6.1.json`). Here that conversion
  produces an unescaped colon, so set `MSYS2_ARG_CONV_EXCL` to `*` to turn it
  off for all arguments.

```bash
MSYS2_ARG_CONV_EXCL="*" \
    ./ffmpeg.exe -i dist.y4m -i ref.y4m \
    -filter_complex "[0:v][1:v]vmafx=model='path=D\\\:/mypath/vmaf_v0.6.1.json'" -f null -
```

## External resources

See [external-resources.md](external-resources.md) for a list of FFmpeg-based
third-party tools.

## History

- **Vulkan removed ([ADR-0726](../adr/0726-drop-vulkan-backend.md), [ADR-2166](../adr/2166-ffmpeg-series-drops-vulkan-shims.md)).**
  Legacy Vulkan filters, options, and configure flags no longer exist. ADR-2166 superseded ADR-0860,
  completely removing the legacy no-op Vulkan shims and enabling a clean patch series renumbering from `0001`.
- **`vmafx`, `vmafx_tune`, `vmafx_pre` and `-vmafx-profile` redesigned ([ADR-2125](../adr/2125-vmafx-ffmpeg-gstreamer-filters.md), [ADR-2166](../adr/2166-ffmpeg-series-drops-vulkan-shims.md)).**
  Patches `0001` to `0006` define the unified VMAFx filter series, dropping all modifications to `vf_libvmaf.c`
  and retiring all fork-specific `libvmaf_*` filters in favor of `--enable-libvmafx`.
