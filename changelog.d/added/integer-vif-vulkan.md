Add `integer_vif_vulkan` feature extractor source (`core/src/feature/vulkan/integer_vif_vulkan.c`),
aligning the Vulkan backend naming convention with the CUDA counterpart
(`feature/cuda/integer_vif_cuda.c`). The file replaces `vif_vulkan.c` in the
Meson build and is wired into `feature_extractor.c` as `vmaf_fex_integer_vif_vulkan`
(registration unchanged). GLSL shaders `vif.comp` / `vif_reduce.comp` and the
two-level GPU reduction (ADR-0350) are unmodified.
