- **Vulkan `integer_vif` shader memory-model race in cross-subgroup
  int64 reduction.** `core/src/feature/vulkan/shaders/vif.comp`
  used bare `barrier()` calls between the cooperative shared-memory
  writes (Phase-1 tile load, Phase-2 vertical convolution, Phase-4
  cross-subgroup `s_lmem` accumulator) and the corresponding reads
  in the next phase. At the default Vulkan 1.3 `apiVersion` the gate
  is 0/48 at `places=4` on all backends shipped today; at the
  in-flight Vulkan 1.4 bump (T-VK-1.4-BUMP / Step B) NVIDIA's
  stricter default memory model surfaces a real race in the Phase-4
  reduction — `(num_scale2, den_scale2)` come back with
  non-deterministic 10¹¹× magnitudes + sign flips, the score
  collapses to 1.0 via the host `den <= 0` fallback, and the
  cross-backend gate fails 45/48 frames on `integer_vif_scale2`.
  Fix: replace each bare `barrier()` with the explicit
  `memoryBarrierShared(); barrier();` pair, which expands to SPIR-V
  `OpControlBarrier` with
  `gl_StorageSemanticsShared | gl_SemanticsAcquireRelease`
  shared-memory release-acquire semantics. Applied uniformly to all
  SCALE values (the structural race lives in the code shared by all
  four pipeline specialisations; SCALE = 2 is just the smallest
  workgroup count where the hardware schedule made the bug
  observable). Verified on real hardware (NVIDIA RTX 4090 + driver
  595.71.05 + Vulkan instance loader 1.4.341 + local API-1.4 bump):
  `places=4` gate 0/48 across all 4 scales, 5-run deterministic
  scale-2 `(num, den) = (+2.494358e+04, +2.522523e+04)` matching the
  CPU reference. RADV (Mesa 26.1.0) was already clean and stays
  clean. Intel Arc A380 (Mesa-ANV / DG2) at API 1.4 still exhibits
  the same residual signature (`T-VK-VIF-1.4-RESIDUAL-ARC` Open) —
  Phase-3b will explore stronger fences. Netflix golden gate
  unaffected (Vulkan code path is independent of the 3 CPU
  goldens). See research-0089 2026-05-09 status appendix for the
  empirical numbers and the corrected device-map attribution.
