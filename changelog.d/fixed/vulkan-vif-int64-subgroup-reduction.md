- Replaced Vulkan VIF's Phase-4 `subgroupAdd(int64_t)` accumulator
  reductions with a manual `subgroupShuffleXor` butterfly, closing the
  NVIDIA API-1.4 `integer_vif_scale2` parity residual without regressing
  Arc or RADV.
