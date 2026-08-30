- **`ffmpeg-patches/0008-add-libvmaf_tune-filter.patch` migrated to
  `ff_filter_link()` for FFmpeg n7+ compat.** `AVFilterLink::frame_rate`
  was removed in FFmpeg n7; the replacement is the new
  `FilterLink` struct accessed via `ff_filter_link(AVFilterLink *)`
  (defined in `libavfilter/filters.h`). Sibling patches 0005
  (`vf_libvmaf_sycl.c`) and 0006 (`vf_libvmaf_vulkan.c`) already used
  the post-n7 accessor; only patch 0008 was written against the n6-era
  API and slipped through CI because the FFmpeg-Vulkan lane only builds
  `vf_libvmaf.o`, not `vf_libvmaf_tune.c`. The full SYCL lane now
  catches it (path-filter from PR #415 includes `ffmpeg-patches/**`).
  `config_output()` in `vf_libvmaf_tune.c` now does
  `ff_filter_link(outlink)->frame_rate = ff_filter_link(mainlink)->frame_rate;`
  to mirror 0005/0006. Series replays clean against pristine `n8.1`;
  `vf_libvmaf_tune.o` builds green. Discovery: PR #415 / ADR-0317.
  Originating patch ADR: ADR-0312.
