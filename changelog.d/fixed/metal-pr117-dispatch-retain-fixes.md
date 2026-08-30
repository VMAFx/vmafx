**Metal dispatch table + ARC retain-balance fixes (PR #117 audit MT-1 + MT-2)**

- MT-1 (`dispatch_strategy.c`): `g_metal_features[]` was missing
  `"float_ms_ssim_metal"` — `vmaf_metal_dispatch_supports()` returned 0
  for that extractor name, silently falling back to CPU on Apple Silicon
  even after the `float_ms_ssim_metal` kernel was wired in (ADR-0490 /
  T-VULKAN-METAL-DEAD-SCAFFOLDS-2026-05-18). Entry added adjacent to
  `"float_ms_ssim"`.
- MT-2 (`picture_import.mm`): `vmaf_metal_state_init_external` applied
  `CFRetain()` **and** `__bridge_retained` to both `device` and `queue`,
  accumulating +2 retains per handle. `vmaf_metal_state_free` only applied
  one `__bridge_transfer` (-1 retain), leaking one Obj-C reference per
  init/close cycle. `CFRetain` calls removed; `__bridge_retained` alone
  is the correct single ownership transfer.
