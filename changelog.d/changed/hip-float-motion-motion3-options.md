- **`float_motion_hip` emits `motion3` and takes every CPU `float_motion`
  option (ADR-1404).** The HIP twin wrote `motion` and `motion2` only and
  lacked `motion_blend_factor` (`mbf`), `motion_blend_offset` (`mbo`),
  `motion_filter_size` (`mfs`), `motion_add_scale1` (`mdc`) and
  `motion_add_uv` (`mau`), so `--backend hip --feature float_motion` dropped
  `motion3` from the output and a request with one of those options was
  computed on the CPU. It now emits `VMAF_feature_motion3_score` with the
  CPU's blend, selects the blur filter in the kernel, adds the half-size SAD
  with a second kernel and runs both on the U and V planes for
  `motion_add_uv`. On a gfx1036 every option is within 1e-5 of the CPU on the
  Netflix 576x324 pair and a 3840x2160 clip, and `motion` / `motion2` with the
  previous options are bit-identical to the previous build.
