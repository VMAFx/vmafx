## docs(usage,api): correct backend auto-priority + Doxygen drift in public headers

- `docs/usage/vmafx-cli.md`: corrected backend auto-priority table from stale
  `CUDA > Vulkan > SYCL > CPU` to the libvmaf registry order `SYCL > CUDA > HIP > CPU`
  (Vulkan dropped per ADR-0726).
- `docs/usage/vmaf-tune-score-backend.md`: added callout explaining that vmaf-tune's
  `auto` probe order (`cuda → sycl → hip → cpu`) differs from the libvmaf registry
  order (`sycl → cuda → hip → cpu`).
- `docs/usage/vmaf-tune.md`: same callout; removed stale `vulkan` backend from the
  mode table, detection heuristics list, and wall-clock table; removed `vulkan` from
  the `--backend` help-line example.
- `core/include/libvmaf/libvmaf.h`: fixed `vmaf_write_output_with_format` @brief —
  the default format for a NULL `score_format` is `"%.6f"` (not `"%.17g"`); added
  `@param` tags to that function; removed stale `Vulkan` from `VmafContext` typedef doc.
- `core/include/libvmaf/libvmaf_hip.h`: improved `@return` doc on
  `vmaf_hip_import_state` to enumerate all error codes.
- `core/include/libvmaf/AGENTS.md`: added `libvmaf_metal.h` and `libvmaf_mcp.h`
  rows to the Scope table.
- `core/include/libvmaf/model.h`: replaced the non-Doxygen `@field` pattern in
  `VmafModelConfig` with per-member block comments (per ADR-0953 / AGENTS.md rule).
- `docs/usage/bench.md`: added HIP to the backend-comparison sentence in the intro.
- `docs/usage/ffmpeg.md`: updated quick-start examples to use `model=version=` form
  (preferred) over legacy `model_path=`; noted `model_path` as legacy; removed stale
  Vulkan references from the Background section and Selector option reference table.
