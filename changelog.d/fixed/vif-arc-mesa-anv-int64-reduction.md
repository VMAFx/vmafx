- **Vulkan `integer_vif` API-1.4 residual: Phase 3b stronger-fence
  experiments + hardware-mapping correction.** Phase 3b tested three
  candidate stronger-fence variants on top of PR #511's
  `memoryBarrierShared(); barrier();` baseline, against the
  Phase-4 cross-subgroup int64 reduction site in
  `core/src/feature/vulkan/shaders/vif.comp`: (C1) `shared coherent`
  / `shared volatile` qualifiers — _not buildable_, glslc 2026.1
  rejects with "memory qualifiers cannot be used on this type" (per
  GLSL 4.50 §4.10, those qualifiers apply to buffer + image variables
  only); (C2) `subgroupMemoryBarrierShared()` immediately after the
  elected-thread shared write block — _builds, no effect_; (C3)
  device-scope `controlBarrier(gl_ScopeWorkgroup, gl_ScopeDevice,
  gl_StorageSemanticsShared, gl_SemanticsAcquireRelease)` — _builds,
  no effect_. C2 + C3 stacked also tested for completeness, same
  outcome. Hardware-mapping correction lands at the same time:
  re-baselining at API 1.4 with PR #511's fix in place on this
  session's multi-GPU host (NVIDIA RTX 4090 + Intel Arc A380 + AMD
  RADV/CPU) shows `--vulkan_device 0` is **NVIDIA RTX 4090**, not Arc
  A380, contrary to PR #511's commit message. The 45/48 / 1.527e-02
  / 5-run-non-deterministic signature reported as "Arc-only" is
  actually NVIDIA-only; Arc is already 0/48 5-run-deterministic.
  `vmaf_vulkan_context_new`'s device sort is stable inside the same
  `devtype_score` bucket and the `vkEnumeratePhysicalDevices` order
  is host-policy-dependent. Outcome: state.md row
  `T-VK-VIF-1.4-RESIDUAL-ARC` retired (the Arc lane is clean) and
  replaced by `T-VK-VIF-1.4-RESIDUAL-NVIDIA-DEFERRED`. Per
  `feedback_no_test_weakening` the `places=4` gate is not relaxed
  and the API-1.4 bump (Step B) stays blocked until either a manual
  int64 lane-by-lane subgroup-reduction patch lands in `vif.comp`
  (replaces the `subgroupAdd(int64_t)` call site, which the working
  hypothesis localises as the bug surface — see research-0090
  §"Hypotheses") or NVIDIA ships a driver-side fix. The shipping
  default is API 1.3 where the gate is **0/48 on every device**
  (NVIDIA + Arc + RADV all verified end-of-session). Netflix golden
  gate unaffected — Vulkan code path is independent of the 3 CPU
  goldens. See research-0090 for the full empirical record.
