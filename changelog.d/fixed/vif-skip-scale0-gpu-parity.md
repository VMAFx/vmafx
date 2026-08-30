# fix(cuda,sycl,vulkan): wire `vif_skip_scale0` option into GPU VIF backends (ADR-0468)

`--feature vif=vif_skip_scale0=true` was silently ignored on CUDA, SYCL, and
Vulkan backends: the struct field existed but the option was not registered in the
extractor's `options[]` table and the collect path did not honour it.  GPU backends
now emit `0.0` for `VMAF_integer_feature_vif_scale0_score` and exclude scale 0 from
the aggregate when the flag is set — matching the CPU `integer_vif.c` behaviour.
