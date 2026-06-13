- chore(test): register 4 orphan tests + fix HIP suite asymmetry in
  `core/test/meson.build`. `test_context` and `test_log` were declared as
  `executable()` but never registered with `test()` — VmafContext lifecycle
  and `log.c` coverage was silently dead. `test_version.c` and
  `test_vif_skip_scale0.c` were orphan source files (compiled into nothing) —
  now wired as `executable()` + `test()` under suite `['fast']`.
  `test_hip_motion3_parity` and `test_hip_adm_parity` registered with only
  `['gpu']` while CUDA/SYCL twins use `['fast', 'gpu']`; aligned to
  `['fast', 'gpu']` to match the cross-backend parity test convention.
  Scope-excludes `test_cambi_vulkan.c` + `test_psnr_vulkan_chroma_geom.c`
  (DRAFT PR #299 deletes them).
