### fix(sycl,vulkan): wire vif_skip_scale0 score suppression in integer_vif GPU twins

`integer_vif_sycl` and `vif_vulkan` both accepted the `vif_skip_scale0` option
in their options tables but never applied it: scale-0 accumulated into
`score_num`/`score_den` and the per-scale score was emitted without
suppression, diverging from the CPU reference when the option was set.

Both backends now match `integer_vif.c write_scores()` exactly:
- `vif_scale0_score` → `0.0` when `vif_skip_scale0=true`
- scale-0 excluded from the combined `score_num`/`score_den` totals
- debug path: `num_scale0 → 0.0`, `den_scale0 → -1.0` (sentinel)

The SYCL backend also gains the missing struct field and options-table entry
(previously absent from `integer_vif_sycl`'s options array entirely).
The Vulkan backend gains the struct field, options-table entry, and the same
score-suppression wiring.
