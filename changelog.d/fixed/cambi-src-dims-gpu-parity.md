- `cambi` CUDA, SYCL, and Vulkan backends now accept `src_width` and `src_height`
  options (aliases `srcw` / `srch`), matching the CPU backend's option surface.
  Previously these fields existed in the state struct but were always overridden
  from the actual input dimensions, making any user-supplied value unreachable.
  A value of 0 (default) continues to resolve to the input dimensions at `init()`
  time. The options are forward-compatible with a future `full_ref` GPU port;
  they have no effect on the current GPU v1 code paths.
