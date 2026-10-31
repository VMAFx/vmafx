<!-- markdownlint-disable MD013 MD046 MD060 -->
# Using VMAFx with GStreamer

The `vmafx` element scores a distorted video against a reference with the [VMAFx C API](../api/vmafx/index.md) and passes the distorted buffers downstream. VMAFx extends Netflix's Video Multi-Method Assessment Fusion (VMAF) library; this element is its native GStreamer integration. Its always-present sink pads are `reference` and `distorted`; its output pad is `src` ([element and pad definitions](../../gstreamer/src/gstvmafx.c)).

## Build the element {#build}

Build or install `libvmafx` first using the [source-build guide](../getting-started/index.md#build-from-source-any-platform), choosing the library backends you need. The element is a separate Meson project: building `core/` alone does not build it. Its [build definition](../../gstreamer/meson.build) requires Meson >= 1.1, `gstreamer-1.0`, `gstreamer-base-1.0` and `gstreamer-video-1.0` >= 1.18, `gmodule-2.0`, and `libvmafx` through pkg-config.

With an installed library visible to pkg-config, run from the repository root:

```bash
meson setup build-gst gstreamer
ninja -C build-gst
```

For an existing uninstalled library build at `build/`, use the `meson-uninstalled` directory named in [meson.build](../../gstreamer/meson.build); [run.sh](../../gstreamer/test/run.sh) uses the same layout:

```bash
PKG_CONFIG_PATH="$PWD/build/meson-uninstalled" meson setup build-gst gstreamer
ninja -C build-gst
export GST_PLUGIN_PATH="$PWD/build-gst"
export LD_LIBRARY_PATH="$PWD/build/src${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
gst-inspect-1.0 vmafx
```

The plugin target is `gstvmafx`; its installation directory is GStreamer's pkg-config `pluginsdir`, falling back to Meson's `libdir/gstreamer-1.0`. Optional CUDA and Vulkan input support is configured separately from the library's scoring backends ([meson.build](../../gstreamer/meson.build)).

| Meson option | Type | Default | Allowed values / effect |
| --- | --- | --- | --- |
| `cuda` | feature | `auto` | `auto`, `enabled`, `disabled`; requires `gstreamer-cuda-1.0` >= 1.22 and `cuda.h` to enable CUDA input |
| `toolkit_root` | string | empty | CUDA toolkit directory containing `include/cuda.h`; also searches compiler include paths, `/opt/cuda`, `/usr/local/cuda`, `/usr` |
| `vulkan` | feature | `auto` | `auto`, `enabled`, `disabled`; requires `gstreamer-vulkan-1.0` >= 1.22 and `vulkan/vulkan.h` |
| `plugin_license` | string | `EUPL-1.2` | Registration string; keep the default for this plugin's licence |

Source: [meson_options.txt](../../gstreamer/meson_options.txt) and the dependency/header checks in [meson.build](../../gstreamer/meson.build). For a build that requires both hardware input paths, replace the setup command above with:

```bash
PKG_CONFIG_PATH="$PWD/build/meson-uninstalled" meson setup build-gst gstreamer \
  -Dcuda=enabled -Dvulkan=enabled -Dtoolkit_root=/usr/local/cuda
```

Use `-Dcuda=disabled -Dvulkan=disabled` to build only the system-memory input path. `auto` can omit an optional path when its dependencies are missing; `enabled` requires it. The CUDA header failure is `feature cuda: cuda.h was not found; set -Dtoolkit_root`; the Vulkan header failure is `feature vulkan: vulkan/vulkan.h was not found` ([meson.build](../../gstreamer/meson.build)). The declared dependency minimum is not a claim that every Vulkan pipeline was tested on GStreamer 1.22; check the installed build and driver.

## Score two videos {#first-pipeline}

Link the inputs by pad name. Frames pair in arrival order, without timestamp matching: supply corresponding frames in the same order and align the content upstream. Both inputs must have the same pixel format, width, height and memory kind ([`aggregate_pair`, `no_pair`](../../gstreamer/src/gstvmafx.c), [`pads_differ`](../../gstreamer/src/gstvmafx_caps.c)).

This CPU example takes two raw I420 files of the stated dimensions and frame rate:

```bash
gst-launch-1.0 -m \
  filesrc location=ref.yuv ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 ! v.reference \
  filesrc location=dist.yuv ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 ! v.distorted \
  vmafx name=v backend=cpu model=version=vmaf_v0.6.1 log-path=score.json score-fmt=%.17g ! fakesink
```

`-m` exposes the [bus messages](#messages). `log-path` requests an end-of-stream report; `score-fmt=%.17g` preserves double precision in report and window-file numbers. The example follows the pipeline in [`gst_launch` and `ParityCpu`](../../gstreamer/test/test_gst_vmafx_parity.py), whose assertions compare reports with the CLI at `--precision max`; those assertions are a reproducible check, not a report of a new hardware run.

With `model` unset, [`load_models`](../../gstreamer/src/gstvmafx_run.c) loads the library default unless `feature` is set, in which case only the named features run. The current default is `vmaf_v1.0.16_3d0h` ([`VMAF_DEFAULT_MODEL_VERSION`](../../core/include/libvmaf/model.h)); explicitly selecting `vmaf_v0.6.1` makes the comparison with upstream's default model intentional.

### CUDA memory {#cuda}

Use a library built with CUDA scoring support and a plugin built with CUDA input support. The uploader branches below follow [`Cuda.element_cuda`](../../gstreamer/test/test_gst_vmafx_parity.py):

```bash
gst-launch-1.0 -m \
  filesrc location=ref.yuv ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 \
    ! cudaupload ! 'video/x-raw(memory:CUDAMemory)' ! v.reference \
  filesrc location=dist.yuv ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 \
    ! cudaupload ! 'video/x-raw(memory:CUDAMemory)' ! v.distorted \
  vmafx name=v backend=cuda model=version=vmaf_v0.6.1 log-path=score.json score-fmt=%.17g ! fakesink
```

[`gstvmafx_cuda.c`](../../gstreamer/src/gstvmafx_cuda.c) imports device pointers without downloading pixels, orders reads after the producer's CUDA stream with an acquire event, and retains buffers until the release fence orders their reuse. `backend=auto` selects CUDA for CUDA memory and CPU for system memory ([`resolve_backend`](../../gstreamer/src/gstvmafx_run.c)).

`import=host` explicitly maps CUDA input for host access and logs `import=host: device frames are downloaded to system memory`. It does **not** change the selected scoring backend to CPU: backend resolution remains separate. `host-copy-frames` counts these downloads for both inputs, rather than counting pairs ([frame mapping](../../gstreamer/src/gstvmafx_frame.c), [backend selection](../../gstreamer/src/gstvmafx_run.c)).

### Vulkan images {#vulkan}

Vulkan is an input memory kind, not a `backend` property value. With `backend=auto`, NVIDIA images select CUDA, AMD images select HIP, and other vendors select SYCL; the scoring device must match the Vulkan GPU's PCI location ([`gst_vmafx_backend_for_vulkan`, `pick_vulkan_device`](../../gstreamer/src/gstvmafx_run.c), [ADR-2152](../adr/2152-vmafx-vulkan-frame-import.md)). Use `import=auto`: this path refuses both `host` and `device`.

This H.264 example follows the decoder branch of [`VulkanFrames`](../../gstreamer/test/test_gst_vmafx_parity.py). `vulkanh264device1dec` is an example factory for GPU index 1, not a portable factory name: confirm it with `gst-inspect-1.0 vulkanh264device1dec` and substitute the decoder factory available on the intended GPU. Both streams must decode to matching caps.

```bash
gst-launch-1.0 -m \
  filesrc location=ref.h264 ! h264parse ! vulkanh264device1dec \
    ! 'video/x-raw(memory:VulkanImage)' ! v.reference \
  filesrc location=dist.h264 ! h264parse ! vulkanh264device1dec \
    ! 'video/x-raw(memory:VulkanImage)' ! v.distorted \
  vmafx name=v backend=auto import=auto model=version=vmaf_v0.6.1 log-path=score.json ! fakesink
```

Keep the Vulkan producer linked so its downstream context query reaches `vmafx`. The element offers `gst.vulkan.instance` and `gst.vulkan.device` contexts with `VK_KHR_external_memory_fd`. It copies pixels on the GPU into exportable per-plane images and waits on the host for that GPU operation; it does not download pixels. Planar CUDA imports may require a further device copy. `host-copy-frames=0` therefore describes absence of a host download, not absence of GPU copies ([`can_export`, `copy_into`, `import_slot`, context-query handling](../../gstreamer/src/gstvmafx_vulkan.c)).

### Scoring while encoding {#encode-time}

To score decoded encoder output against the source, branch CUDA frames with `tee` and feed the decoder output to `distorted`. The example uses the topology in [`Cuda.encode_time_pipeline`](../../gstreamer/test/test_gst_vmafx_parity.py); the test's extra download branches collect comparison fixtures, while scoring itself uses CUDA memory.

```bash
gst-launch-1.0 -m \
  filesrc location=src.yuv ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 \
    ! videoconvert ! video/x-raw,format=NV12 ! cudaupload ! 'video/x-raw(memory:CUDAMemory)' ! tee name=u \
  u. ! queue ! v.reference \
  u. ! queue ! nvh264enc ! h264parse ! nvh264dec ! 'video/x-raw(memory:CUDAMemory)' ! queue ! v.distorted \
  vmafx name=v backend=cuda model=version=vmaf_v0.6.1 n-stats-frames=24 stats-out=log+metadata ! fakesink
```

The reference and decoded streams must still correspond by frame order; window times follow the distorted stream's PTS, including encoder timestamp offsets ([pairing](../../gstreamer/src/gstvmafx.c), [`Cuda.test_encode_time_scoring_equals_files`](../../gstreamer/test/test_gst_vmafx_parity.py)).

## Accepted caps and refusals {#caps}

The format lists below come from [`formats` and `gst_vmafx_template_caps`](../../gstreamer/src/gstvmafx_caps.c), shared by both sink pads and `src`. All advertised structures have width and height `1..2147483647` and framerate `0/1..2147483647/1`; these are negotiation ranges, not measured device limits. The selected library backend, device and extractors must also accept the frames.

| Input caps | Formats | Scoring path / restrictions |
| --- | --- | --- |
| `video/x-raw` (system memory) | `I420`, `Y42B`, `Y444`, `GRAY8`, `I420_10LE`, `I420_12LE`, `I422_10LE`, `I422_12LE`, `Y444_10LE`, `Y444_12LE`, `Y444_16LE`, `GRAY16_LE`, `NV12`, `P010_10LE` | CPU by default; explicit CUDA, SYCL or HIP uses that library backend. Planar frames are wrapped read-only; NV12/P010 use host import, subject to the host-binding check. `import=device` refuses system memory. |
| `video/x-raw(memory:CUDAMemory)` | `I420`, `Y444`, `NV12`, `P010_10LE` | Advertised only with CUDA input support; requires `backend=cuda` or `auto`. `auto`/`device` import the device memory; `host` requests a download. |
| `video/x-raw(memory:VulkanImage)` | `I420`, `Y444`, `NV12`, `P010_10LE` | Advertised even without Vulkan support so negotiation can name the refusal. Requires a Vulkan-enabled plugin, `import=auto`, a non-CPU backend and a matching GPU. |
| `video/x-raw(memory:GLMemory)` | `I420`, `Y444`, `NV12`, `P010_10LE` | Advertised to issue an explicit refusal; no GL import yet. |
| `video/x-raw(memory:DMABuf)` | None | Absent from the pad template; no direct GstDMABuf import path. Library dma-buf support used by Vulkan/SYCL/HIP does not add these caps to the element. |

Sources: [caps checks](../../gstreamer/src/gstvmafx_caps.c), [host mapping/import](../../gstreamer/src/gstvmafx_frame.c), [CUDA import](../../gstreamer/src/gstvmafx_cuda.c), [Vulkan import](../../gstreamer/src/gstvmafx_vulkan.c). A decoder negotiating system memory may download before this element; its downloads are outside `host-copy-frames`.

The following are literal error templates from [caps validation](../../gstreamer/src/gstvmafx_caps.c); `<pad>`, `<format>`, `<backend>` and dimensions stand for the runtime substitutions:

| Refused case | Error text |
| --- | --- |
| Unsupported format reaching validation | `pad <pad>: format <format> is not one the element scores` |
| Mismatched inputs (including memory kind) | `pad <pad>: format <format> <w>x<h> differs from the other pad's format <other-format> <other-w>x<other-h> (the inputs must match)` |
| Software frames with `import=device` | `pad <pad>: format <format> in system memory: import=device refuses software frames` |
| Semi-planar host binding unavailable | `pad <pad>: format <format> in system memory: the <backend> device does not bind host memory for semi-planar frames` |
| CUDA with a different backend | `pad <pad>: format <format> in CUDAMemory: backend <backend> does not read it (use backend cuda or auto)` |
| Vulkan without build support | `pad <pad>: format <format> in VulkanImage: this build has no GStreamer Vulkan support (the element never downloads device memory)` |
| Vulkan with CPU or `import=host`/`device` | `pad <pad>: format <format> in VulkanImage: backend cpu and import=host or device do not take it (the element never downloads device memory)` |
| GL memory | `pad <pad>: format <format> in GLMemory: not imported yet (GL import needs the WP3 GL interop lanes; on AMD negotiate DMABuf); the element never downloads a frame to score it` |

The GL error's DMABuf suggestion does not establish direct DMABuf support: the pad template above is authoritative. DMABuf and D3D11 memory have no dedicated refusal text in this source; negotiation can reject caps before the element's validation runs. Malformed caps that reach the handler produce `pad <pad>: the caps are not raw video` ([sink event handler](../../gstreamer/src/gstvmafx.c)).

A Vulkan producer without export support fails with `the Vulkan device of these frames cannot export memory (it was not made by this element: link the element directly downstream of the Vulkan element, or share a device with VK_KHR_external_memory_fd)`, prefixed with the pad name. Missing PCI information produces `pad reference: the Vulkan device reports no PCI location (VK_EXT_pci_bus_info), so its GPU cannot be matched` ([Vulkan pool setup](../../gstreamer/src/gstvmafx_vulkan.c), [session setup](../../gstreamer/src/gstvmafx_run.c)).

## Properties {#properties}

These are all element-defined properties, including aliases. Names, defaults and ranges come from the [shared option table](../../ffmpeg-patches/src/vf_vmafx_options.h), generated from [`core/api/vmafx.toml`](../../core/api/vmafx.toml). [`gst_vmafx_opts_install` / `make_pspec`](../../gstreamer/src/gstvmafx_opts.c) replace `_` with `-` and expose named enums and flag sets as **strings**, not GObject enum/flags types. Set properties before scoring (`GST_PARAM_MUTABLE_READY`). Inherited GStreamer properties are outside this table; inspect the installed element for them.

`gstreamer/test/test_gst_vmafx_properties.py` holds the element's own property
list and the table below to the definition: an extra or a missing property, in
the element or in this table, fails the test (`gstreamer/test/run.sh` runs both
halves; the required `GStreamer Element` check runs that script).

`I64` below means `0..9223372036854775807` (`INT64_MAX`); string properties with no default show `NULL`. Named values also accept integers in their table range; flag sets combine tokens with `+` (or use `0` for an empty set). Runtime checks can reject a value within its declared property range ([parser](../../gstreamer/src/gstvmafx_opts.c), [session validation](../../gstreamer/src/gstvmafx_run.c)).

| Property | GObject type | Default | Allowed values / use |
| --- | --- | --- | --- |
| `model` | string | `NULL` | Colon-delimited model spec: `version=`, `path=`, `name=`, `disable_clip`, `enable_transform`, feature option overrides; separate models with `\|`, maximum 16. Unset selects the library default unless `feature` is set. |
| `backend` | string | `auto` | `auto`, `cpu`, `cuda`, `sycl`, `hip`, `metal` (numeric `0..5`); Metal is refused. Auto follows the input memory as described above. |
| `feature` | string | `NULL` | Feature specs `name[=key=value:...]`, separated by `\|`; for example `psnr` or `cambi=full_ref=true`. |
| `tiny-model` | string | `NULL` | Tiny ONNX model path, loaded alongside the selected classic models. |
| `tiny-device` | string | `auto` | `auto`, `cpu`, `cuda`, `openvino`, `openvino-npu`, `openvino-cpu`, `openvino-gpu`, `coreml`, `coreml-ane`, `coreml-gpu`, `coreml-cpu`, `rocm` (numeric `0..11`); availability depends on the library. |
| `tiny-threads` | int64 | `0` | I64; CPU execution-provider intra-op threads, `0` uses runtime default; passed to the library capped at `INT32_MAX`. |
| `tiny-fp16` | boolean | `false` | `false`, `true`; request fp16 input/output when the provider supports it. |
| `threads` | int64 | `0` | I64; extractor workers, `0` scores in the calling thread; passed to the context capped at `UINT32_MAX`. |
| `n-threads` | int64 | `0` | Alias of `threads`, same range and storage. |
| `cpumask` | int64 | `0` | I64; mask of CPU instruction sets to disable. |
| `gpumask` | int64 | `0` | I64; mask of GPU operations to disable. |
| `device` | string | `auto` | `auto` or decimal device index `0..2147483647`; CPU requires `auto`, CUDA memory uses its buffer context, Vulkan selects by PCI location. |
| `subsample` | int64 | `1` | `1..INT64_MAX`; score every Nth frame. |
| `n-subsample` | int64 | `1` | Alias of `subsample`, same range and storage. |
| `log-path` | string | `NULL` | Report filename; unset requests no report. Written at EOS. |
| `log-fmt` | string | `json` | `json`, `xml`, `csv`, `sub` (numeric `0..3`). |
| `view-distance` | double | `0.0` | `0..24` display heights; `0` leaves the model's ADM distance unchanged. The option table warns that default ADM CSF refuses distances below 3. |
| `display-height` | int64 | `0` | I64; reference display height in pixels; `0` leaves the model's value unchanged. |
| `target-width` | int64 | `0` | I64; nonzero is currently refused. |
| `target-height` | int64 | `0` | I64; nonzero is currently refused. |
| `target-scaling` | string | `none` | `none`, `bilinear`, `bicubic`, `lanczos` (numeric `0..3`); anything other than `none` is currently refused. |
| `pool` | string | `mean` | `min`, `max`, `mean`, `harmonic_mean`, `median`, `perc5`, `perc10`, `perc20`, combined with `+` (numeric mask `0..255`); empty mask is refused. |
| `n-stats` | double | `0.0` | `0..DBL_MAX` seconds; positive enables time windows; exclusive with positive `n-stats-frames`. |
| `n-stats-frames` | int64 | `0` | I64; positive enables frame-count windows; exclusive with positive `n-stats`. |
| `stats-out` | string | `log` | `log`, `metadata`, `file`, combined with `+` (numeric mask `0..7`); `file` requires `stats-path`. |
| `stats-path` | string | `NULL` | Window NDJSON filename; used with `stats-out=file`. |
| `score-fmt` | string | `%.6f` | One `%[flags][width][.precision]` conversion ending in `e`, `E`, `f`, `F`, `g` or `G`; e.g. `%.17g`. Controls report/NDJSON formatting, not bus double values. |
| `provenance` | string | `log+report` | `log`, `report`, combined with `+` (numeric mask `0..3`); see [provenance](#provenance). |
| `metadata` | boolean | `false` | `false`, `true`; post final per-frame model scores as `vmafx-frame` bus messages. |
| `profile` | boolean | `false` | `false`, `true`; sets `VMAFX_DEVICE_PROFILING` on device creation; the element defines no separate timing message. |
| `import` | string | `auto` | `auto`, `device`, `host` (numeric `0..2`); memory-specific restrictions are in [accepted caps](#caps). |
| `perceptual-weight` | boolean | `false` | `false`, `true`; `true` is currently refused. |

Element-specific behavior is in [`load_models`, `load_features`, `load_tiny`, `create_device`, `create_context`, `check_options`](../../gstreamer/src/gstvmafx_run.c); score format syntax is checked by [`gst_vmafx_score_fmt_ok`](../../gstreamer/src/gstvmafx_util.c). Model and feature spec parsing belongs to the library ([ADR-2125](../adr/2125-vmafx-ffmpeg-gstreamer-filters.md)). Quote multi-item arguments in a shell, for example `'feature=psnr|float_ssim|float_ms_ssim'`.

Non-default tiny options without `tiny-model` fail with `options tiny-device, tiny-threads, tiny-fp16 need tiny-model`. Feature-only sessions cannot request model-score windows or per-frame messages: `options n-stats, n-stats-frames, metadata pool the scores of a model: set model` ([session validation](../../gstreamer/src/gstvmafx_run.c)).

## Windows and per-frame scores {#windows}

For example, replace the `vmafx` line in the CPU pipeline with:

```bash
  vmafx name=v backend=cpu model=version=vmaf_v0.6.1 pool=min+mean+harmonic_mean \
    n-stats-frames=10 stats-out=log+metadata+file stats-path=windows.ndjson \
    metadata=true provenance=log+report log-path=score.json score-fmt=%.17g ! fakesink
```

`n-stats` uses the distorted buffer's PTS in nanoseconds, falling back to the frame index and negotiated framerate, or zero without a rate. `n-stats-frames` uses frame count. The library window clock supplies boundaries and the partial-tail flag; a full frame-count window is not partial. Time windows follow actual timestamps, so rounding can put 13 frames in a 0.5-second window at 24 fps ([`buffer_pts_ns`](../../gstreamer/src/gstvmafx.c), [window handling](../../gstreamer/src/gstvmafx_windows.c), [`Windows.test_time_windows`](../../gstreamer/test/test_gst_vmafx_parity.py), [ADR-2074](../adr/2074-vmafx-window-scores.md)).

`stats-out=metadata` posts window messages; `metadata=true` independently requests per-frame messages at the `subsample` interval. The element polls completed results while feeding pairs and drains them after flush, so bus delivery need not coincide with downstream delivery of that frame. `stats-out=log` writes INFO records to the `vmafx` debug category; `stats-out=file` writes and flushes one NDJSON object per window. NDJSON uses `window`, `start`, `end`, `n_frames`, `n_scored`, `partial` and nested `<model>: {<method>: <score>}` objects ([output](../../gstreamer/src/gstvmafx_out.c), [window emission](../../gstreamer/src/gstvmafx_windows.c)).

## Messages {#messages}

These are `GST_MESSAGE_ELEMENT` messages; their structure names and complete element-defined fields are listed below. Scores use the loaded model name (for example `vmaf`, or a `model` spec's `name=`). Pool-method suffixes are `min`, `max`, `mean`, `harmonic_mean`, `median`, `perc5`, `perc10`, `perc20`, restricted to the chosen `pool` set ([output](../../gstreamer/src/gstvmafx_out.c), [window messages](../../gstreamer/src/gstvmafx_windows.c), [method names](../../gstreamer/src/gstvmafx_util.c)).

| Structure name | When | Fields and GTypes |
| --- | --- | --- |
| `vmafx-provenance` | First pair, after session creation, if `provenance` includes `log` | `json`: `G_TYPE_STRING` |
| `vmafx-frame` | Scored frame becomes final and is drained, if `metadata=true` | `index`: `G_TYPE_UINT64`; `<model>`: `G_TYPE_DOUBLE` for each model |
| `vmafx-window` | Completed window is drained, if `stats-out` includes `metadata` | `window`: `G_TYPE_UINT64`; `start`, `end`: `G_TYPE_DOUBLE` in seconds; `n_frames`, `n_scored`: `G_TYPE_UINT64`; `partial`: `G_TYPE_BOOLEAN`; `<model>.<method>`: `G_TYPE_DOUBLE` |
| `vmafx-summary` | Successful EOS flush and result drain | `n_frames`, `host-copy-frames`: `G_TYPE_UINT64`; `<model>.<method>`: `G_TYPE_DOUBLE` |

`n_frames` in the summary counts submitted pairs; `host-copy-frames` counts CUDA input buffers explicitly mapped for host access. Frame messages have no timestamp/duration fields and no `type` field. The final summary is posted **before** the report is written, so it does not prove report-file success; watch for a subsequent `GST_MESSAGE_ERROR` ([`gst_vmafx_finish_stream`](../../gstreamer/src/gstvmafx_out.c)).

## Provenance {#provenance}

`provenance=log` both logs the JSON at INFO and posts `vmafx-provenance`; `report` requests the CSV/SUB sidecar `<log-path>.provenance.json`. JSON and XML reports embed provenance regardless of this flag, so `provenance=0` does not remove their record. `log-path` is required to write any report. The initial bus record is read before the first submission; use the final report for the completed run's record ([`gst_vmafx_post_provenance`, `write_report`](../../gstreamer/src/gstvmafx_out.c), [library report writer](../../core/src/vmafx/report.c), [provenance guide](provenance.md)).

## Moving from upstream's `vmaf` element {#migration}

The upstream names and defaults in this table are documented on the [GStreamer gst-plugins-bad `vmaf` page](https://gstreamer.freedesktop.org/documentation/vmaf/index.html) (checked 2026-10-28). The destination properties are defined in the [shared option table](../../ffmpeg-patches/src/vf_vmafx_options.h) and implemented by [session setup](../../gstreamer/src/gstvmafx_run.c). Migration requires changing pad names and message consumers as well as the factory name.

| Upstream property / surface | `vmafx` migration |
| --- | --- |
| `model-filename` (default `vmaf_v0.6.1`) | `model=version=vmaf_v0.6.1` for that built-in model, or `model=path=/path/model.json`; do not rely on the different library default |
| `disable-clip` | Append `:disable_clip` to the selected `model` spec |
| `enable-transform` | Append `:enable_transform` to the selected `model` spec |
| `phone-model` | No same-named property; gap: the cited upstream page does not specify a model-spec translation, so no equivalence is claimed |
| `conf-interval` | No corresponding element property |
| `psnr`, `ssim`, `ms-ssim` | `feature=psnr`, `feature=float_ssim`, `feature=float_ms_ssim`, or a quoted `\|`-separated list; also set `model` to retain classic model scoring |
| `pool-method` | `pool`; same names `min`, `max`, `mean`, `harmonic_mean`, with additional methods and `+` combinations |
| `results-filename` | `log-path` |
| `results-format` | `log-fmt=json`, `xml` or `csv`; upstream `none` has no format value here, so omit `log-path` to request no report |
| `frame-message` | `metadata=true`; consume `vmafx-frame` using the schema above |
| `subsample` | `subsample` (default `1`) |
| `threads` | `threads` (default `0`); here `0` means scoring in the calling thread, whereas upstream documents automatic threads |
| `log-level` | No corresponding property; library logging uses the `vmafx` category, e.g. `GST_DEBUG=vmafx:4` for INFO |
| `ref_sink`, `dist_sink` pads | `reference`, `distorted` |
| Reference video output; `VMAF` bus structure | Distorted video output; separate `vmafx-frame`, `vmafx-window`, `vmafx-summary`, `vmafx-provenance` structures |

Pipelines that keep upstream's `vmaf` element keep working: a gst-plugins-bad
build against the stock `libvmaf.h` loads this project's `libvmaf.so.3`
compatibility library and produces the same scores, value for value, in every
output format and with every property above. CI holds this on each library
change ([upstream consumers](../development/upstream-consumers.md)). The
compatibility library follows the timeline of
[ADR-1852](../adr/1852-vmafx-api-redesign.md) decision D7: available in 1.0,
the `vmafx` element is the default from 1.1, and `libvmaf.so.3` is removed in
2.0. Move to `vmafx` before 2.0.

The feature-spec examples are also used in the [FFmpeg property migration guide](ffmpeg.md); library logging is routed by [`gst_vmafx_log_callback`](../../gstreamer/src/gstvmafx_util.c). Only upstream properties listed on the cited page are mapped here.

## Limits and diagnostics {#limits}

- Metal is an accepted property token but fails at start with `backend metal: this element imports CPU, CUDA and Vulkan memory; Metal comes with its WP3 lane` ([`gst_vmafx_check_backend`](../../gstreamer/src/gstvmafx_caps.c)).
- D3D11, Metal-backed memory and VAMemory are absent from [`gst_vmafx_template_caps`](../../gstreamer/src/gstvmafx_caps.c) and the memory kinds in [`GstVmafxMem`](../../gstreamer/src/gstvmafx.h). There is no D3D11-specific error string to quote; no direct import support is claimed. GL's literal source comment is `advertised; refused until the GL import lane` ([memory enum](../../gstreamer/src/gstvmafx.h)).
- Nonzero target display options fail with `options target-width, target-height, target-scaling: scoring on a target display lands in RC5`; perceptual weighting fails with `option perceptual-weight: GstBuffers carry no perceptual side data` ([`check_unsupported`](../../gstreamer/src/gstvmafx_run.c)).
- When one input ends, unmatched buffers are dropped with `dropping an unpaired buffer: the other input ended`. They are not passed downstream or counted as scored pairs. Empty input never creates a scoring session, summary or report ([`no_pair`, `finish`](../../gstreamer/src/gstvmafx.c)). This differs from FFmpeg's pairing contract; [ADR-2165](../adr/2165-vmafx-filter-pairing-contract.md) explicitly leaves GStreamer pairing unchanged.
- Reports are written at successful EOS, not on an early stop. A bad report path can fail after scoring and after the summary message; it is not probed at startup ([`write_report`, `gst_vmafx_finish_stream`](../../gstreamer/src/gstvmafx_out.c)).
- Dolby Vision profile 5 sources require conversion outside the pipeline; the integration does not apply RPU data ([ADR-2125, decision 8](../adr/2125-vmafx-ffmpeg-gstreamer-filters.md)).
- The plugin registers `EUPL-1.2` ([build option](../../gstreamer/meson_options.txt), [`GST_PLUGIN_DEFINE`](../../gstreamer/src/gstvmafx.c)). The option's source description warns that GStreamer loads it but logs an unknown-licence warning at debug level 2; this is not a reason to change its licence string.

## Reproduce the integration checks {#checks}

[`gstreamer/test/run.sh`](../../gstreamer/test/run.sh) builds the plugin against existing library builds and runs [`test_gst_vmafx_parity.py`](../../gstreamer/test/test_gst_vmafx_parity.py):

```bash
gstreamer/test/run.sh "$PWD/build-cpu" "$PWD/build-cuda"
```

The defaults are `build-cpu` and `build-cuda`; missing GStreamer development packages produce exit 77. CPU tests need fixtures and the matching library/CLI; CUDA tests need the CUDA build and GStreamer CUDA package. Vulkan tests additionally require the `GST_VMAFX_VK_*`, `VMAFX_VK_LIB_DIR` and `VMAF_VK_CLI` variables documented by `VulkanFrames`. The suite skips unavailable fixtures/lanes, so inspect skips before treating a run as backend verification. `GST_VMAFX_STRICT=1` turns a missing CPU fixture or `gi` module into a failure, and `GST_VMAFX_PYTHON` names the interpreter that has PyGObject; the required `GStreamer Element` check sets both ([ADR-3390](../adr/3390-gstreamer-element-check.md)). It covers CLI parity, windows, messages, provenance and selected refusals; its Vulkan uploader branch includes a GStreamer 1.28 staging-race workaround. This page's pipelines are derived from those tests; no fresh CPU/GPU execution result is implied.
