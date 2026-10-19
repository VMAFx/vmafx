## VMAFx FFmpeg filters and the loopback-decoder hwaccel patch (2026-10-18)

WP9 ([ADR-2125](adr/2125-vmafx-ffmpeg-gstreamer-filters.md)) adds four
patches to the FFmpeg series: `ffmpeg-patches/0021-add-vmafx-filter.patch`
(`vmafx` and `vmafx_tune`), `0022-add-vmafx-pre-filter.patch` (`vmafx_pre`),
`0023-add-vmafx-profile-cli-glue.patch` (`-vmafx-profile`) and
`0024-fftools-loopback-decoder-hwaccel.patch` (`-hwaccel*` before `-dec`).
Files added: the sources under `ffmpeg-patches/src/` (`vf_vmafx.c`,
`vf_vmafx_pre.c`, `vf_vmafx_options.h`, `ffmpeg_dec_hwaccel.c`), the check
`ffmpeg-patches/test/vmafx_filter_check.py`, the GStreamer element under
`gstreamer/`, and `core/src/vmafx/spec.c` (`vmafx_model_load_spec()` and
`vmafx_context_use_feature_spec()`).

Invariants a rebase keeps:

- The sources under `ffmpeg-patches/src/` are the truth. Each patch carries a
  copy; `python3 scripts/ci/ffmpeg_patch_stack.py --refresh` amends the owning
  patch from the source, and `--check` replays the series. Never edit the
  copy inside a patch by hand.
- `ffmpeg-patches/src/vf_vmafx_options.h` and the option table in
  `docs/usage/ffmpeg.md` are generated from `core/api/vmafx.toml`
  (`scripts/codegen/vmafx-api.py --write`); on a conflict take either side and
  regenerate.
- Host frames are wrapped on the CPU device (`host_wrap()` calls
  `vmafx_frame_wrap_host()` with no device, in `vf_vmafx.c`), whatever device
  the context scores on; software frames are wrapped without a copy.
- The refusals stay refusals: a frame the library cannot import fails the graph
  naming the backend, and a fixed-size frame pool smaller than
  `vmafx_context_max_in_flight() + 1` is refused before any frame. A
  silent fallback to the host is a regression.
- Vulkan frames (`vk_*()` in `vf_vmafx.c`, under `CONFIG_VULKAN`) are copied
  on the GPU into the filter's own pool of exportable per-plane frames and
  imported on the device at the Vulkan device's PCI location (request
  WP3-vulkan-1, ADR-2152). The filter loads its Vulkan functions through the
  device's `get_proc_addr` and fetches its queue with `vkGetDeviceQueue2()`
  and the device's `queue_flags`; FFmpeg's `vulkan.o` helpers are not linked,
  so the filter's objects stay buildable without `--enable-vulkan`.
- A device frame returns to its producer only after its release fence: the
  CUDA release event, the copies' timelines on CUDA, or a HOST release fence
  that `box_release_resources()` waits on (DRM PRIME, Vulkan on SYCL and HIP).
- Patch `0024` is carried by the fork only (decision Q-048). When an FFmpeg
  release changes `fftools/ffmpeg_dec.c` or `ffmpeg_demux.c`, the patch is
  refreshed, not dropped.
- The `libvmaf*` patches `0001` to `0020` are untouched until WP10.

`vmafx_filter_check.py` (`parity`, `windows`, `provenance`, `refusal`, `pool`,
`legacy`, `e2e`) guards the behaviour on a device; the generated-table drift
check guards the options. No score or public C API impact beyond the spec
functions of ABI 0.1.11.
