Fix five wrong or spurious entries in the Metal backend dispatch support table
(`core/src/metal/dispatch_strategy.c`). Three feature-key strings did not
match the values emitted by their extractors' `provided_features[]` arrays,
causing `vmaf_metal_dispatch_supports()` to silently return false for
`float_motion` motion/motion2, `integer_motion` motion2, and
`integer_motion_v2` motion2_v2 on Apple Silicon. Two spurious entries
(`"motion3_score"`, `"float_ms_ssim"`) were also removed; they allowed
incorrect Metal-dispatch routing for features that have no Metal
implementation.
