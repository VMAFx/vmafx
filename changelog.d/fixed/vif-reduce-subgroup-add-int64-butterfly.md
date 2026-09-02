Replace `subgroupAdd(int64_t)` with the XOR-swap butterfly (`reduce_i64_subgroup`)
in `vif_reduce.comp` to match the fix already applied to `vif.comp` Phase-4.
Closes the residual NVIDIA Vulkan 1.4 non-determinism tracked as
`T-VK-VIF-1.4-RESIDUAL` (ADR-0454 / ADR-0269 / research-0090).
