## Vulkan integer ADM opt-in path (2026-11-07)

- `core/src/feature/vulkan/` (host TU + shaders), `core/src/vulkan/`
  (runtime subset), `core/src/feature/vulkan/probe/`,
  `core/test/test_vulkan_{probe,integer_adm_parity}.c` +
  `core/test/data/vulkan-adm/`: restored from the pre-ADR-0726 snapshot
  `816a5f33f` and reconciled to the fork PoC `poc/vk-integer-adm` @
  `0bb98ee21`. **On sync**: upstream Netflix/vmaf has none of this; do
  not port blindly.
- `core/src/feature/vulkan/shaders/integer_adm.comp` carries the six
  ADR-3402 porting deviations. The two that differ from the PoC on
  purpose: the scale-0 CM own-tap keeps int32 per **ADR-1402** (the PoC
  kept the Netflix-style `int16_t` wrap; both CPU anchors agree on the
  gate fixtures), and the angle predicate runs fp64-free via
  `adm_angle_flag_i64()` (research-2030) so no shader requires
  `shaderFloat64`. **On rebase**: if ADR-1402's upstream status changes
  (Netflix/vmaf#1602 merges or is withdrawn), re-check deviation 2
  before touching the wrap; do not restore `subgroupAdd(int64_t)`
  (ADR-3402 bans it outright).
- The conclude path calls the CPU's own shared inlines
  (`integer_adm_kernels.h` / `adm_score.h` / `nonfinite_score.h`). If
  the CPU integer ADM conclude changes shape, the Vulkan twin follows
  automatically for those parts, but the committed parity references
  under `core/test/data/vulkan-adm/refs/` must be re-derived at the new
  anchor and the GPU gate re-run.
- `core/meson_options.txt` `enable_vulkan` (feature, default
  `disabled`): CI never enables it. If upstream gains its own
  `enable_vulkan`, this option's description and the guard in
  `core/src/meson.build` must be reconciled before merge.
