**integer_vif CUDA: add missing `vif_skip_scale0` option** — the CUDA twin of
`integer_vif` accepted `vif_skip_scale0` in neither its options table nor its
collect path. Scale-0 accumulated and emitted unmodified, diverging from the
CPU reference and the SYCL/Vulkan twins fixed in PR #1057. The option is now
registered and the `write_scores()` path suppresses scale-0 score (emitting
`0.0`), excludes scale-0 num/den from combined totals, and emits the `0.0 /
-1.0` debug sentinels, matching `integer_vif.c write_scores()` exactly.
