<!-- markdownlint-disable MD012 MD013 MD024 MD060 -->
# AGENTS.md — libvmaf/src/vulkan (REMOVED — historical reference)

> **The Vulkan backend was removed per [ADR-0726](../../../docs/adr/0726-drop-vulkan-backend.md)
> (2026-05-28).** The source files in this directory are orphan code no longer
> compiled by any meson target. The `enable_vulkan` meson option, the public
> header `libvmaf_vulkan.h`, and the CI lanes have been removed. Do not add
> new code here. The content below is preserved for historical context.

---

Orientation for agents working on the Vulkan compute backend
runtime (historical). Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

The Vulkan-side runtime (instance / device / queue creation,
memory allocator via VMA, picture lifecycle, dispatch helpers,
zero-copy `VkImage` import). Vulkan **feature kernels** live in
[../feature/vulkan/](../feature/vulkan/) (`.c` host glue +
`shaders/*.comp` GLSL).

```text
vulkan/
  common.c              # VkInstance / VkPhysicalDevice / VkQueue
  vulkan_common.h       # public-internal types
  vulkan_internal.h     # private helpers
  picture_vulkan.c/.h   # VmafPicture on a Vulkan device
  import.c              # zero-copy VkImage / external-memory import
  import_picture.h      # per-state staging buffers + ImageToBuffer copy
  dispatch_strategy.c/.h  # PRIMARY_CMDBUF vs SECONDARY_CMDBUF_REUSE
  vma_impl.cpp          # AMD Vulkan Memory Allocator translation unit
  spv_embed.py          # build-time helper: SPV → C array embed
  meson.build
```

Public header: [`include/libvmaf/libvmaf_vulkan.h`](../../include/libvmaf/libvmaf_vulkan.h).

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **Volk dynamic loader** is bundled. All `vk*` symbols are
  remapped to `vmaf_priv_vk*` at the C preprocessor level via a
  force-included header — see
  [ADR-0185](../../../docs/adr/0185-vulkan-hide-volk-symbols.md) +
  [ADR-0198](../../../docs/adr/0198-volk-priv-remap-static-archive.md) +
  [ADR-0200](../../../docs/adr/0200-volk-priv-remap-pkgconfig-leak-fix.md).
  Both the shared `.so` and static `.a` builds drop zero `vk*`
  GLOBAL symbols; this is load-bearing for BtbN-style fully-static
  FFmpeg link environments.
- **`enable_vulkan` meson option** defaults to `disabled`. The
  scaffold from [ADR-0175](../../../docs/adr/0175-vulkan-backend-scaffold.md)
  has zero runtime dependencies; the runtime PR
  ([ADR-0127](../../../docs/adr/0127-vulkan-compute-backend.md)) wires
  `dependency('vulkan')` + volk + glslc + VMA.
- **GLSL FMA contraction is OFF for precision-critical kernels.**
  Compile shaders with `-O0` or use `precise` / `NoContraction`
  decorations on the load-bearing accumulators (matches CUDA
  `--fmad=false` and SYCL `-fp-model=precise`). Whenever you add
  a new GPU twin, run the cross-backend gate at the contracted
  `places` precision target before declaring success.
- **`precise` is NOT a substitute for the cross-backend gate on
  NVIDIA at API ≥ 1.4** ([ADR-0264](../../../docs/adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md),
  [ADR-0269](../../../docs/adr/0269-vif-ciede-precise-step-a.md),
  [research-0054](../../../docs/research/0056-vif-ciede-precise-step-a-implementation.md)):
  driver 595.71 has been observed to drift on `vif.comp` scale-2
  (45/48 mismatches, max abs `1.527e-02`) under a 1.4 bump *despite*
  every load-bearing FP op being correctly decorated with
  `OpDecorate ... NoContraction` (verified at the SPIR-V `OpFDiv` /
  `OpFMul` / `OpFSub` ID level). Either the driver doesn't honour
  the decoration on those ops, or the regression is in something
  other than FMA contraction. Do not assume `precise` alone makes
  the 1.4 bump safe — re-run `/cross-backend-diff` against the
  actual NVIDIA lane after any change that touches a float-heavy
  shader. The four `apiVersion` sites
  ([`common.c:54/264/374`](common.c) + [`vma_impl.cpp:22`](vma_impl.cpp))
  were bumped to `VK_API_VERSION_1_4` in Step B (DRAFT PR — see
  ADR-0264 status appendix 2026-05-09); the bump is held behind
  Phase 3c (PR #512) until the NVIDIA `subgroupAdd(int64_t)`
  workaround closes the residual `integer_vif_scale2` 45/48 gap.
- **Conservative `precise` scope on `ciede.comp` is empirically
  optimal**: widening it into the helper functions (`get_h_prime`,
  `get_upcase_t`, `get_r_sub_t`, `srgb_to_linear`, `xyz_to_lab_map`)
  or onto the Lab axes makes the cross-backend gate strictly worse
  on NVIDIA (5/48 → 46/48 mismatches). The shader carries inline
  comments recording this empirical bound; do not widen the scope
  without re-measuring against the NVIDIA lane.

## Rebase-sensitive invariants

- **VkImage zero-copy import surface
  ([ADR-0184](../../../docs/adr/0184-vulkan-image-import-scaffold.md) +
  [ADR-0186](../../../docs/adr/0186-vulkan-image-import-impl.md))**:
  `vmaf_vulkan_import_image` / `vmaf_vulkan_wait_compute` /
  `vmaf_vulkan_read_imported_pictures` /
  `vmaf_vulkan_state_init_external` /
  `vmaf_vulkan_state_build_pictures` are public symbols consumed
  by `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`.
  Touching the surface requires updating that patch in the same
  PR (CLAUDE.md §12 r14). Synchronous v1 design: per-state ref/dis
  staging `VkBuffer` pair (HOST_VISIBLE, DATA_ALIGN-strided),
  `vkCmdCopyImageToBuffer` + timeline-semaphore wait per frame.
  Async pending-fence v2 + true zero-copy GPU compute deferred.
- **Header purity**: VkImage / VkSemaphore / VkDevice cross the
  ABI as `uintptr_t` (mirrors `libvmaf_cuda.h`). Do not include
  `<vulkan/vulkan.h>` from the public header.
- **Per-state dispatch strategy
  ([ADR-0181](../../../docs/adr/0181-feature-characteristics-registry.md))**:
  `dispatch_strategy.c` consumes the per-feature
  `VmafFeatureCharacteristics` descriptor + frame dims + env
  override (`VMAF_VULKAN_DISPATCH=feature:strategy,...`) and
  returns `PRIMARY_CMDBUF` vs `SECONDARY_CMDBUF_REUSE` (stub —
  reuse path lands with T7-18 follow-up).
- **GPU long-tail terminus** — every registered feature
  extractor now has at least one Vulkan twin. Cambi was the
  terminus, integrated by PR #196 (T7-36, ADR-0210 placeholder)
  in the hybrid host/GPU pattern from
  [ADR-0205](../../../docs/adr/0205-cambi-gpu-feasibility.md).
  Future feature additions need a same-PR Vulkan twin OR an
  explicit ADR-recorded deferral.
- **Cross-backend gate
  ([ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md))**:
  the lavapipe lane (`vulkan-parity-matrix-gate`) runs on every
  PR. Adding a new GLSL kernel requires a `FEATURE_TOLERANCE`
  entry if it relaxes places=4, plus a row in
  [`docs/development/cross-backend-gate.md`](../../../docs/development/cross-backend-gate.md).
- **Instance / VMA Vulkan API version bumped to 1.4 (Step B,
  gated on Phase 3c)
  ([ADR-0264](../../../docs/adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md))**:
  the four sites — `apiVersion` on lines 54, 264, 374 of
  `common.c` and `VMA_VULKAN_VERSION` (`1004000`) on line 22 of
  `vma_impl.cpp` — now request Vulkan 1.4. The Step-A precise
  audit (PR #346) and the Phase-3 cross-subgroup release-acquire
  fix (PR #511) closed Arc + RADV at places=4. NVIDIA driver
  595.71.05 + RTX 4090 still fails `integer_vif_scale2` 45/48
  (max abs 1.527e-02) until Phase 3c (PR #512) lands the
  `subgroupAdd(int64_t)` workaround. Step B's PR is DRAFT and
  block-on-merge until Phase 3c is green on the NVIDIA lane.
  Captured originally in
  [research-0053](../../../docs/research/0053-vulkan-1-4-nvidia-fp-contraction-regression.md);
  status update in
  [research-0089 2026-05-09 appendix](../../../docs/research/0089-vulkan-vif-fp-residual-bisect-2026-05-08.md).
  Compiled SPIR-V is byte-identical at vulkan1.3 vs vulkan1.4;
  the regression is purely runtime, mediated by core
  `shaderFloatControls2` on Vulkan 1.4.

- **`vif.comp` SCALE = 2 cross-subgroup reduction is a memory-model
  hot-spot at API 1.4** ([research-0089
  2026-05-09 status appendix](../../../docs/research/0089-vulkan-vif-fp-residual-bisect-2026-05-08.md)):
  Phase 2 dynamic dump on RTX 4090 + driver 595.71.05 + Vulkan
  1.4.341 localised the T-VK-VIF-1.4-RESIDUAL failure to the
  Phase-4 cross-subgroup int64 reduction (`vif.comp` lines 547–
  592, `subgroupAdd` + `barrier()` + thread-0 read of `s_lmem`).
  Empirical signature on NVIDIA at API 1.4 is **non-deterministic**
  `den_scale2` ~ −10¹⁶ vs CPU's +2.5×10⁴ (run-to-run distinct,
  10¹¹× magnitude flip + sign flip), score collapses to 1.0 via
  the `den <= 0` host fallback in `reduce_and_emit()`. Bug is
  isolated to the SCALE = 2 specialisation; scales 0/1/3 stay
  deterministic + sane on the same machine. **The earlier
  hypothesis attributing the residual to `shaderFloatControls2`
  v2 codegen is refuted** — no FP-precision flip can synthesise
  10¹¹× amplification, and the FP-arithmetic surface in vif.comp
  is exhausted (5 ops, all `NoContraction`-decorated, verified at
  the SPIR-V `OpFDiv` / `OpFMul` / `OpFSub` ID level). When
  Phase 3 lands, the fix replaces bare `barrier()` with explicit
  `controlBarrier(gl_ScopeWorkgroup, gl_ScopeWorkgroup,
  gl_StorageSemanticsShared, gl_SemanticsAcquireRelease)` (or
  `memoryBarrierShared() + barrier()`) before the thread-0
  reduction read, and gates on a 5-run determinism check in
  addition to `places=4`. The `places=3` NVIDIA-only override
  path is **not viable** for this bug — non-deterministic
  accumulators cannot meet any tolerance.

- **Phase 3 update 2026-05-09 — `vif.comp` shared-memory
  release-acquire fix landed.** All three bare `barrier()` calls
  in `vif.comp` (Phase-1 cooperative tile load, Phase-2
  vertical-conv shared write, Phase-4 cross-subgroup int64
  reduction) are now `memoryBarrierShared(); barrier();` pairs.
  **Rebase invariant:** when porting upstream changes that touch
  `vif.comp`, do *not* downgrade these pairs back to bare
  `barrier()` — the NVIDIA 1.4 race will return.

- **Phase 3b update 2026-05-09 — hardware-mapping correction +
  stronger-fence experiments concluded.** Re-baselining at API 1.4
  with PR #511's fix in place on the session's multi-GPU host
  (NVIDIA RTX 4090 + Intel Arc A380 + AMD RADV/CPU) showed the
  PR #511 / research-0089 device-map attribution was inverted.
  `vmaf_vulkan_context_new`'s device sort is stable inside the
  same `devtype_score` bucket and `vkEnumeratePhysicalDevices`
  order is host-policy-dependent (driver registration / Mesa
  device-select layer / loader env vars), so `--vulkan_device 0`
  is **not** portable across hosts. On this session's host
  device 0 = NVIDIA RTX 4090 (still 45/48 FAIL scale 2 post-#511),
  device 1 = Intel Arc A380 + Mesa-ANV (0/48 OK), device 2 = RADV
  CPU (0/48 OK). The "Arc residual" PR #511 opened is therefore a
  phantom; the actual remaining residual is on **NVIDIA**.
  Phase 3b tested three stronger-fence candidates on top of #511 —
  `shared coherent` (not buildable, GLSL 4.50 §4.10),
  `subgroupMemoryBarrierShared()` (builds, no effect),
  device-scope `controlBarrier` (builds, no effect) — and stacked
  C2+C3 for good measure. None closed the residual. State.md row
  retired as `T-VK-VIF-1.4-RESIDUAL-NVIDIA-DEFERRED`; working
  hypothesis is a driver bug in NVIDIA's int64 emulation of
  `subgroupAdd(int64_t)` for SCALE=2. **Rebase invariant for
  cross-backend gate authors:** select Vulkan devices by
  `deviceName` substring, not by `--vulkan_device` index — index
  is host-policy-dependent. The shipping default is API 1.3
  where the gate is 0/48 on every device. See research-0090.

- **Phase 3c update 2026-05-14 — `vif.comp` manual int64 subgroup
  reduction closes the NVIDIA API-1.4 VIF residual.** Do not replace
  the seven Phase-4 `reduce_i64_subgroup(...)` calls in
  `../feature/vulkan/shaders/vif.comp` with `subgroupAdd(int64_t)`.
  The manual `subgroupShuffleXor` butterfly is load-bearing on
  NVIDIA RTX 4090 + driver 595.71.05 at Vulkan API 1.4: it changes
  `integer_vif_scale2` from 45/48 failures at max abs `1.527e-02`
  to 0/48 at max abs `2.000000e-06`, while Arc A380 and RADV stay
  0/48. See research-0108 and ADR-0269 Phase-3c status update.

## Governing ADRs

- [ADR-0127](../../../docs/adr/0127-vulkan-compute-backend.md) —
  Vulkan compute backend strategic decision.
- [ADR-0175](../../../docs/adr/0175-vulkan-backend-scaffold.md) —
  scaffold-only audit-first PR (T5-1).
- [ADR-0176](../../../docs/adr/0176-vulkan-vif-cross-backend-gate.md) —
  Vulkan VIF cross-backend gate (lavapipe + Arc A380 lanes).
- [ADR-0177](../../../docs/adr/0177-vulkan-motion-kernel.md),
  [ADR-0178](../../../docs/adr/0178-vulkan-adm-kernel.md),
  [ADR-0185](../../../docs/adr/0185-vulkan-hide-volk-symbols.md),
  [ADR-0186](../../../docs/adr/0186-vulkan-image-import-impl.md),
  [ADR-0187](../../../docs/adr/0187-ciede-vulkan.md)..
  [ADR-0201](../../../docs/adr/0201-ssimulacra2-vulkan-kernel.md) —
  per-kernel records.
- [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  GPU-parity CI gate.
- [ADR-0273](../../../docs/adr/0273-ciede-vulkan-nvidia-f32-f64-precision-gap.md) —
  ciede2000 NVIDIA-Vulkan places=4 5/48 fork debt is a structural
  f32/f64 colour-space-chain precision gap (CPU `get_lab_color`
  runs in `double`, shader runs in `float`). **Do not** promote
  the ciede shader to f64 — `shaderFloat64` is optional and runs
  at 1/64 fp32 throughput on RTX 4090. **Do not** narrow the CPU
  reference to f32 — that changes Netflix golden ground truth.
  See ADR-0273 + research-0055 before "fixing" the 5/48 tail.

Orientation for agents working on the Vulkan backend runtime. Parent:
[../../AGENTS.md](../../AGENTS.md).

## Layout

The Vulkan-side runtime (instance / device / queue / VMA allocator
lifecycle, picture buffer pools, dispatch helpers, host-side import
slots for VkImage zero-copy). Vulkan **feature kernels** live one
level deeper in [../feature/vulkan/](../feature/vulkan/), with their
GLSL compute shaders under
[../feature/vulkan/shaders/](../feature/vulkan/shaders/).

```text
vulkan/
  common.c               # context init + global state (instance, device, queue)
  dispatch_strategy.c/.h # PRIMARY vs SECONDARY cmdbuf decision

  import.c               # VkImage zero-copy import slots (T7-29)
  import_picture.h       # public-facing import surface
  kernel_template.h      # per-feature Vulkan kernel scaffolding (ADR-0246)
  meson.build            # SPIR-V embed chain (glslc + spv_embed.py)
  picture_vulkan.c/.h    # VmafPicture on a Vulkan device + buffers
  spv_embed.py           # generates per-shader <name>_spv.h byte array
  vma_impl.cpp           # VMA C++17 implementation TU
  vulkan_common.h        # opaque public surface
  vulkan_internal.h      # VmafVulkanContext / State / ImportSlots

  import.c              # VkImage zero-copy import slots
  import_picture.h      # public-facing import surface
  kernel_template.h     # per-feature Vulkan kernel scaffolding (ADR-0246)
  meson.build           # SPIR-V embed chain (glslc + spv_embed.py)
  picture_vulkan.c/.h   # VmafPicture on a Vulkan device + buffers
  spv_embed.py          # generates per-shader <name>_spv.h byte array
  vma_impl.cpp          # VMA allocator instantiation TU
  vulkan_common.h       # opaque public surface
  vulkan_internal.h     # context + import-slot layout (kernel-side)

```

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **Every Vulkan call has its `VkResult` checked.** No silent drops;
  feature kernels return `-EIO` / `-ENOMEM` on failure and let the
  feature collector unwind.
- **`volk` loads every Vulkan entry point at runtime** — kernel TUs
  must include `vulkan_internal.h` (which `#define`s
  `VK_NO_PROTOTYPES` and pulls in `<volk.h>`), never `<vulkan/vulkan.h>`
  directly. Otherwise you get duplicate definitions.
- **Numerical snapshots**: kernels that don't bit-match the CPU
  scalar reference at `places=4` regenerate
  `testdata/scores_cpu_vulkan.json` via
  [`/regen-snapshots`](../../../.claude/skills/regen-snapshots/SKILL.md)
  with a justification in the commit message.

## Rebase-sensitive invariants

- **`kernel_template.h` is the canonical kernel scaffolding**
  (fork-local, ADR-0221, extended by ADR-0256): the inline helpers
  `vmaf_vulkan_kernel_pipeline_create/_destroy`,
  `vmaf_vulkan_kernel_submit_begin/_end_and_wait/_free` plus the
  ADR-0256 additions
  `vmaf_vulkan_kernel_submit_pool_create/_destroy/_acquire`,
  `vmaf_vulkan_kernel_descriptor_sets_alloc` capture the
  descriptor-set layout + pipeline layout + shader module + compute
  pipeline + descriptor pool + per-frame command-buffer + fence
  shape every fork-added Vulkan feature kernel uses.

  Submit-pool / descriptor-pre-alloc invariants (ADR-0256):
  1. `VmafVulkanKernelSubmitPool` is created in `init()` with a
     `slot_count` matching the ops-per-frame the kernel issues.
     For single-dispatch kernels (psnr_hvs, vif, adm, motion, psnr,
     float_vif, float_adm) `slot_count = 1`. For multi-fence kernels (e.g.
     ms_ssim's 1 pyramid + 5 SSIM scales = 6) the slot count is
     bounded by `VMAF_VULKAN_KERNEL_POOL_MAX_SLOTS` (8).
  2. `vmaf_vulkan_kernel_submit_acquire` requires the context's
     command pool to carry `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`
     (set in `vulkan/common.c`); `vkResetCommandBuffer(cmd, 0)` is
     the per-acquire entry.
  3. `vmaf_vulkan_kernel_descriptor_sets_alloc` allocates from the
     existing `pl.desc_pool` sized via `max_descriptor_sets` in the
     pipeline-create descriptor; the pool keeps
     `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` so legacy
     callers that still call `vkFreeDescriptorSets` continue to
     work. Pre-allocated sets are destroyed implicitly by
     `vmaf_vulkan_kernel_pipeline_destroy` via the pool — callers
     must NOT call `vkFreeDescriptorSets` on them.
  4. Pre-bound descriptor-set buffers must be init-time-stable —
     i.e. the `VmafVulkanBuffer *` handles do not change between
     `init()` and `close_fex()`. This is the case for every
     migrated kernel today; if a future kernel needs to rebind
     buffers per frame (e.g. dynamic per-scale sizing), it falls
     back to the per-frame `vkUpdateDescriptorSets` pattern. The template
  lands unused — each future kernel migration is its own gated PR
  (`places=4` cross-backend-diff per ADR-0214). The maximum
  SSBO-binding count accepted by `_pipeline_create` is exposed via
  the named constant `VMAF_VULKAN_KERNEL_MAX_SSBO_BINDINGS`
  (currently 16); lift the define if a future kernel exceeds it.
  Both the `desc->ssbo_binding_count` upper-bound check and the
  on-stack `bindings[]` array size must reference the constant —
  never open-code the cap. **On rebase**: keep
  the header and any kernel call-sites that later adopt it; upstream
  has no Vulkan backend at all today, so there is nothing to merge
  against. Reference implementation that mirrors the template's
  shape lives in `libvmaf/src/feature/vulkan/psnr_vulkan.c`. See
  [ADR-0246](../../../docs/adr/0246-gpu-kernel-template.md) and
  [docs/backends/kernel-scaffolding.md](../../../docs/backends/kernel-scaffolding.md).
  Multi-bundle kernels — kernels with several distinct
  descriptor-set-layout shapes (different `ssbo_binding_count`,
  different push-constant struct, or different shader module) — hold
  one `VmafVulkanKernelPipeline` per shape, each with its **own**
  `desc_pool` (the template's `_create()` helper allocates one pool
  per bundle; do not share across bundles). When per-scale or
  per-pass variants alias `bundle->pipeline` into the first slot of
  a `VkPipeline[N]` array, the destroy path must skip slot 0 to avoid
  double-freeing the aliased base via
  `vmaf_vulkan_kernel_pipeline_destroy()`. The 4-bundle layout
  (`pl_xyb` / `pl_mul` / `pl_blur` / `pl_ssim`) in
  `libvmaf/src/feature/vulkan/ssimulacra2_vulkan.c` is the reference
  for this pattern. The 5-bundle layout
  (`pl_trivial` / `pl_derivative` / `pl_filter_mode` / `pl_decimate`
  / `pl_mask_dp`) in `libvmaf/src/feature/vulkan/cambi_vulkan.c`
  (T-GPU-DEDUP-25) is the largest consumer to date — every stage
  uses the same 2-binding SSBO DSL, but distinct per-stage
  push-constant struct sizes force one bundle per stage.

- **Multi-bundle kernels: per-bundle pool + alias-skip on destroy**
  (fork-local, T-GPU-DEDUP-23). Kernels with multiple distinct
  pipeline shapes (different SSBO binding counts → different
  DSLs) can't fit into a single bundle, since `_add_variant()`
  only siblings pipelines under the *same* layout. The pattern
  is: one `VmafVulkanKernelPipeline` per shape (e.g.
  `pl_decimate` + `pl_ssim` in `ms_ssim_vulkan`), and per-frame
  descriptor allocation uses *that bundle's* `desc_pool` + `dsl`
  (no shared pool any more). The first variant slot in each
  per-pipeline-shape array (e.g. `decimate_pipelines[0]`,
  `ssim_pipeline_horiz[0]`) **aliases** the bundle's own
  `pipeline` field — `_pipeline_destroy()` already releases it,
  so `close_fex` must skip those alias slots in its
  `vkDestroyPipeline` loop. Variants destroyed before
  `_pipeline_destroy()`, alias slots skipped, mismatched-pool
  `vkFreeDescriptorSets` is UB. **On rebase**: keep the alias +
  skip pattern verbatim.

- **`VmafVulkanContext` ownership flag** (fork-local, ADR-0186):
  `owns_handles` distinguishes contexts created by libvmaf (true)
  from contexts handed in via the public
  `vmaf_vulkan_state_init_external` (false). `context_destroy` must
  honour the flag — do not unconditionally call `vkDestroyDevice` /
  `vkDestroyInstance`. The VMA allocator and command pool are
  always libvmaf-owned regardless. **On rebase**: keep the flag
  and the conditional destroy logic verbatim.

- **Persistent host-mapped buffers** (fork-local, ADR-0175): every
  feature kernel binds host-visible SSBOs through
  `vmaf_vulkan_buffer_alloc` + `vmaf_vulkan_buffer_host` rather
  than `vkMapMemory` / `vkUnmapMemory` per frame. VMA picks a
  HOST_VISIBLE heap; on non-coherent heaps (some dGPU configs)
  `vmaf_vulkan_buffer_flush` is the explicit coherence point.
  **On rebase**: do not introduce per-frame map/unmap; the
  persistent mapping is the contract.

## Build (historical — no longer functional)

> The `enable_vulkan` meson option was removed in ADR-0726. The commands
> below are preserved for reference only; running them will fail with
> "Unknown option enable_vulkan".

```bash
# HISTORICAL ONLY — will fail with current meson_options.txt:
# meson setup build -Denable_vulkan=enabled
# ninja -C build
```


## Governing ADRs

- [ADR-0175](../../../docs/adr/0175-vulkan-backend-scaffold.md) —
  T5-1 Vulkan backend scaffold.
- [ADR-0186](../../../docs/adr/0186-vulkan-image-import-impl.md) —
  T7-29 VkImage zero-copy import.
- [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  cross-backend `places=4` gate every kernel migration is gated by.
- [ADR-0246](../../../docs/adr/0246-gpu-kernel-template.md) —
  per-feature kernel scaffolding template (`kernel_template.h`).
- **Every Vulkan call has its return checked.** `VK_SUCCESS` is the
  only success value; everything else maps to `-EIO` / `-ENOMEM` /
  `-EINVAL` per the import.c convention.
- **No fence/semaphore handles outlive the device.** Anything created
  via `vkCreate*` must be paired with the matching `vkDestroy*` in
  the teardown path; the teardown order is fixed (see Rebase-sensitive
  invariants below).
- **Public ABI is opaque-handle.** Vulkan handles cross the
  `libvmaf_vulkan.h` ABI as `uintptr_t`; never expose `VkImage` /
  `VkSemaphore` types in the public surface (ADR-0184).
- **Lavapipe is the CI ICD.** Anything that depends on real
  hardware queue concurrency, descriptor-indexing, or driver
  scheduling cannot be CI-gated against lavapipe alone — flag
  the deferred lane and add a hardware verification note.

## Rebase-sensitive invariants

- **Async pending-fence ring (ADR-0251)**: `VmafVulkanImportSlots`
  is a ring of `ring_size` slots keyed by `frame_index %
  ring_size` (default depth 4, max 8 — see
  `VMAF_VULKAN_RING_DEFAULT` / `VMAF_VULKAN_RING_MAX`). Three
  invariants are load-bearing:
  1. Ring depth is fixed at the **first** `vmaf_vulkan_import_image`
     call (when geometry is also pinned). Subsequent calls with
     different `w/h/bpc` return `-EINVAL` — same contract as v1.
     Ring depth is **not** reallocated; if a caller needs a
     different depth they must `vmaf_vulkan_state_free` and re-init.
  2. `vkResetFences` runs **only** after a confirmed `VK_SUCCESS`
     from `vkWaitForFences` — the reset path lives inside
     `drain_slot_fence` and is the only place `fence_in_flight`
     is cleared.
  3. `vmaf_vulkan_state_free` drains every outstanding fence
     before destroying any resource (`vkQueueWaitIdle` belt-
     and-braces in case feature kernels submitted on the same
     queue). Reordering the teardown will leak GPU memory or
     trigger the validation-layer "destroying in-use object"
     error.
- **ABI preservation**: the v2 ring lives entirely inside
  `VmafVulkanState`; the public `libvmaf_vulkan.h` did not change
  signatures across the v1 → v2 swap. ADR-0251 follow-up #3 grew
  `VmafVulkanConfiguration` with `max_outstanding_frames` (additive
  field, zero-init compatible) and added a read-side accessor
  `vmaf_vulkan_state_max_outstanding_frames()`; both keep call-site
  compatibility with v1 callers. The single clamp helper
  `vmaf_vulkan_clamp_ring_size` (now in `vulkan_internal.h`) is the
  one source of truth for the [1, VMAF_VULKAN_RING_MAX] mapping —
  do not duplicate this logic in `state_init` or `lazy_alloc_ring`,
  the value stored in `state->requested_ring_size` must match what
  `lazy_alloc_ring` will use.
- **FFmpeg patch coupling**: any change to the
  `vmaf_vulkan_*` public surface ships the matching update to
  `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`
  in the same PR (CLAUDE.md §12 r14). v2 preserves the ABI so
  the patch is unaffected. ADR-0238 added two entry points
  (`vmaf_vulkan_preallocate_pictures`, `vmaf_vulkan_picture_fetch`)
  and one enum and one struct — purely additive; the FFmpeg patch is
  not consuming them today and remains unchanged.
- **Picture-pool ownership (ADR-0238)**: the
  `VmafVulkanPicturePool` lives on `VmafContext`, not on
  `VmafVulkanState`. The state owns the GPU resources (instance,
  device, queue); the pool borrows the state's
  `VmafVulkanContext` via the fork-internal accessor
  `vmaf_vulkan_state_context()` (declared in `vulkan_internal.h`,
  not the public header). On `vmaf_close()` the pool is closed
  before the state pointer is cleared; ownership of the state is
  still the caller's (matches the SYCL pattern). Do not move the
  pool onto the state — that would force the import-image
  zero-copy path to pay pool costs it doesn't need.
- **Single luma plane only**: the DEVICE pool path allocates one
  VmafVulkanBuffer per picture, sized for the Y plane only
  (matches SYCL). Adding chroma support is a follow-up — when a
  Vulkan extractor actually consumes preallocated U/V planes,
  extend `pool_alloc_one_device` to allocate per-plane buffers
  and bump the buffer-type tag to a chroma-aware variant.

## See also

- [ADR-0186](../../../docs/adr/0186-vulkan-image-import-impl.md)
  — v1 synchronous design.
- [ADR-0251](../../../docs/adr/0251-vulkan-async-pending-fence.md)
  — v2 async ring.
- [Research-0042](../../../docs/research/0042-vulkan-async-pending-fence.md)
  — option-space digest.
- [docs/api/gpu.md](../../../docs/api/gpu.md) — public-API reference.
- [docs/backends/vulkan/overview.md](../../../docs/backends/vulkan/overview.md)
  — backend-level overview.

## Buffer classification invariant (ADR-0357)

Every new `VmafVulkanBuffer` must be allocated with the correct VMA flag:

| Buffer direction                       | Allocation function                        | Post-dispatch action                  |
|----------------------------------------|--------------------------------------------|---------------------------------------|
| UPLOAD — CPU writes, GPU reads         | `vmaf_vulkan_buffer_alloc()`               | `vmaf_vulkan_buffer_flush()` before dispatch |
| READBACK — GPU writes, CPU reads       | `vmaf_vulkan_buffer_alloc_readback()`      | `vmaf_vulkan_buffer_invalidate()` after fence-wait, before `vmaf_vulkan_buffer_host()` |
| GPU-only — neither CPU writes nor reads| either (prefer `alloc_readback` for forward-compatibility) | none required |

**Why this matters on discrete GPUs**: `alloc` uses `VMA_HOST_ACCESS_SEQUENTIAL_WRITE`
(write-combining BAR heap; fast host writes, slow host reads — 4–8× penalty).
`alloc_readback` uses `VMA_HOST_ACCESS_RANDOM` (HOST_CACHED heap preferred;
full host-cache bandwidth on reads).  Using `alloc` for a buffer the CPU must
reduce post-fence silently incurs uncached PCIe reads.

**Invalidate is mandatory on non-coherent heaps** (Vulkan 1.3 spec §11.2.2):
device writes are not visible to the host until `vmaInvalidateAllocation` is
called.  `vmaf_vulkan_buffer_invalidate` is a no-op on HOST_COHERENT heaps
(integrated GPUs, lavapipe), so the call is unconditionally safe.

See [ADR-0357](../../../docs/adr/0357-vulkan-readback-alloc-flag.md) for the
full decision matrix and the per-feature buffer classification table.
