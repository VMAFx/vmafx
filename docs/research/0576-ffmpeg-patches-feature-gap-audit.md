# Research-0576: ffmpeg-patches surface-gap audit (n8.1.1 + feature-exposure)

- **Status**: Active
- **Workstream**: ADR-0576
- **Last updated**: 2026-05-18

## Question

What is the complete gap between libvmaf's public C API surface and what
the `ffmpeg-patches/` stack exposes to FFmpeg users? Does the existing
`feature=` passthrough cover all extractors, or do individual extractors
require dedicated patches?

## Sources

- `core/include/libvmaf/libvmaf.h` — `VmafConfiguration` struct
- `core/include/libvmaf/feature.h` — `VmafFeatureDictionary` API
- `core/include/libvmaf/dnn.h` — DNN session API
- `core/include/libvmaf/libvmaf_cuda.h`, `libvmaf_sycl.h`,
  `libvmaf_vulkan.h`, `libvmaf_hip.h`, `libvmaf_metal.h` — backend APIs
- `core/src/feature/feature_extractor.c` — `feature_extractor_list[]`
- `ffmpeg-patches/` (all 13 patches) — current exposure state
- FFmpeg `libavfilter/vf_libvmaf.c` (upstream n8.1.1 + cumulative patches)

## Findings

### Feature extractor exposure

The upstream FFmpeg `libvmaf` filter exposes a `feature=` AVOption that
calls `vmaf_use_feature(vmaf, name, opts)`. This function calls
`vmaf_get_feature_extractor_by_name(name)` which iterates
`feature_extractor_list[]`. All ~80 extractors registered there (CPU,
CUDA, SYCL, HIP, Vulkan, Metal) are therefore reachable by name from
within ffmpeg without any patch.

Sub-feature options (e.g., `enable_chroma`, `adm_csf_scale`, `scale`)
are passed through the `VmafFeatureDictionary` mechanism via
`feature='name=integer_adm:adm_csf_scale=0.5'` syntax. No patch needed.

### VmafConfiguration gap

The `VmafConfiguration` struct has 5 fields:

- `log_level` — wired via `log_level_map(av_log_get_level())`
- `n_threads` — exposed as `n_threads` AVOption (upstream)
- `n_subsample` — exposed as `n_subsample` AVOption (upstream)
- `cpumask` — **NOT EXPOSED** (gap)
- `gpumask` — **NOT EXPOSED** (gap)

Both `cpumask` and `gpumask` were present since libvmaf 3.0 but never
wired through the filter.

### Symbols audit (Goal B)

The following symbols were named in the task brief as potentially deleted:

- `float_ssim_cuda`: still live — defined in `integer_ssim_cuda.c` (the
  source file was renamed/merged in PR #1343 but the symbol was preserved).
- `adm_hip`, `motion_hip`, `vif_hip`, `integer_ciede_hip`,
  `integer_moment_hip`: still live as `float_adm_hip`, `integer_motion_hip`,
  `integer_vif_hip`, `ciede_hip`, `float_moment_hip` respectively.
- `feature_hip.h`: no such file referenced in any patch.

None of the patches reference `vmaf_fex_*` extractor symbols — they only
use the public C API tier. Zero symbol drops required.

### CLI-only surfaces (not patchable)

- `--precision` (ADR-0119): controls score output format string in the CLI;
  no C API equivalent. The filter emits scores via `av_log()`; format is
  irrelevant.
- `--no-reference`: CLI-only flag that skips reference picture ingestion.
  The filter always ingests two video streams (framesync architecture).
  No C API equivalent.

## Conclusion

The only genuine gap is `cpumask` / `gpumask`. Patch 0014 closes it.
All extractors are already reachable via `feature=name=<extractor>`.
No deleted symbols exist in any patch hunk.
