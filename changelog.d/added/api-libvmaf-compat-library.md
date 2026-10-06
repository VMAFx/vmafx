- **libvmaf is a compat library on the VMAFx API (RC4 WP6).** The engine and
  the VMAFx API ship as `libvmafx.so.1` (pkg-config `libvmafx`), which exports
  `vmafx_*` symbols only; `libvmaf.so.3` keeps the libvmaf API and is written
  on the exported VMAFx functions alone, so `pkg-config --libs libvmaf` now
  gives `-lvmaf -lvmafx`. Programs built for libvmaf, unpatched upstream
  FFmpeg and GStreamer included, build and score unchanged; binaries linked
  against an earlier `libvmaf.so.3` run against this one. The CUDA and SYCL
  functions (HIP and Metal in builds with them) remain the engine's until
  their device-frame work lands. Guide:
  [Migrating from libvmaf.h](docs/api/vmafx/index.md#migrating-from-libvmafh),
  [migration table](docs/api/vmafx/compat.md)
  ([ADR-2094](docs/adr/2094-libvmaf-compat-library-split.md)).
- **VMAFx API 0.1.6 additions** for the compat library
  ([reference](docs/api/vmafx/reference.md)): the libvmaf bridge for pictures,
  models and model sets (`vmafx_frame_from_picture`, `vmafx_frame_to_picture`,
  `vmafx_model_from_libvmaf`, `vmafx_model_libvmaf_handle`,
  `vmafx_model_set_from_libvmaf`, `vmafx_model_set_libvmaf_handle`);
  context-owned preallocated frames (`vmafx_context_preallocate`,
  `vmafx_context_acquire_frame`); `vmafx_context_backend`,
  `vmafx_context_attach_sidedata`, `vmafx_backend_name`; frame converters
  (`vmafx_frame_converter_create`, `vmafx_frame_convert`,
  `vmafx_frame_converter_destroy`); tiny-AI models and sessions
  (`vmafx/dnn.h`: `vmafx_dnn_available`, `vmafx_context_use_tiny_model`,
  `vmafx_context_set_codec_context`, `vmafx_context_is_codec_aware`,
  `vmafx_context_set_tiny_resize`, `vmafx_dnn_session_*`,
  `vmafx_dnn_verify_signature`); the embedded MCP server (`vmafx/mcp.h`:
  `vmafx_mcp_*`); with their structs, handles and constants.
- **Opt-in deprecation warnings for `libvmaf.h`.** Compile with
  `-DVMAF_ENABLE_DEPRECATION_WARNINGS` to have every libvmaf call name its
  VMAFx successor; the warnings become the default in 1.1 and the functions
  go in 2.0 (ADR-1852 decision D7).
- **Upstream consumer conformance.** `scripts/ci/upstream-ffmpeg-compat.sh`
  and `scripts/ci/upstream-gstreamer-compat.sh` build unpatched upstream
  FFmpeg (`FFMPEG_TAG`) and the upstream GStreamer `vmaf` element
  (`GST_PLUGINS_BAD_VERSION`, kept current by Renovate) against an installed
  libvmaf and compare their scores with the `vmaf` command line and with a
  reference library as exact text; the `Upstream Consumers` workflow runs both
  ([guide](docs/development/upstream-consumers.md), #2237).
