<!-- markdownlint-disable MD036 MD060 -->
# ADR-0127: Vulkan compute backend — vendor-neutral GPU path alongside CUDA/SYCL/HIP

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, vulkan, backend, build, agents

## Context

The fork currently ships three GPU runtimes: CUDA (NVIDIA-native,
mature), SYCL (oneAPI/icpx, works on Intel and NVIDIA via the open
codeplay backend), and HIP (AMD ROCm). Each was added to cover a
hardware vendor gap the previous one couldn't reach — SYCL after
CUDA for Intel GPUs, HIP after SYCL for AMD consumer cards where
ROCm on Windows/Linux is uneven.

Three gaps remain:

1. **macOS**: Apple does not ship CUDA, and SYCL / HIP neither
   compile nor run meaningfully on M-series hardware. The only
   cross-vendor compute API macOS supports is Metal (proprietary)
   or Vulkan via MoltenVK (open).
2. **Mobile / embedded**: Vulkan is the only compute API in broad
   reach on Android, mobile Linux, and embedded ARM platforms.
3. **Consumer AMD / Intel GPUs on Windows**: HIP-on-Windows is
   flaky outside ROCm-officially-supported parts; SYCL on Windows
   needs oneAPI runtime packaging; Vulkan Just Works via the
   vendor-shipped graphics driver that every consumer machine
   already has.

A Vulkan compute backend is the single addition that closes all
three gaps with one SPIR-V shader set. The cost is real — Vulkan
compute means hand-writing a dispatcher, queue / command-buffer /
fence management, descriptor-set layout, and (most importantly)
SPIR-V kernels — but the payoff is that the fork becomes runnable on
every consumer GPU sold since 2017 plus all Apple Silicon, without
asking users to install a vendor SDK.

The existing backend tree under [`core/src/`](../../core/src/)
has converged on a consistent shape: each backend has a runtime
directory (`cuda/`, `sycl/`, etc.), per-feature kernel source trees
under `feature/<backend>/`, a public header (`libvmaf_cuda.h`,
`libvmaf_sycl.h`) and a Meson flag (`enable_cuda`, `enable_sycl`).
The [add-gpu-backend](../../.claude/skills/add-gpu-backend/SKILL.md)
skill scaffolds exactly this shape for a new backend.

## Decision

We will add a **Vulkan compute backend** using
[`add-gpu-backend vulkan`](../../.claude/skills/add-gpu-backend/SKILL.md)
with the following constraints:

- **Loader**: [volk](https://github.com/zeux/volk) (single-header
  Vulkan loader). Avoids the `libvulkan.so` hard link-time dep and
  mirrors how the CUDA backend uses a dlopen shim for `libcuda.so.1`
  (see [ADR-0122](0122-cuda-gencode-coverage-and-init-hardening.md)).
- **Shader language**: GLSL 4.60 compute shaders, compiled to
  SPIR-V 1.3 at build time with `glslc` (ships with the Vulkan SDK
  and every Linux distro; also available via `shaderc` as a vendor-
  able dependency). Evaluated alternatives: HLSL via DXC (sharper
  tooling on Windows but foreign to the Linux-first maintainers) and
  Slang (interesting but too new for production).
- **Memory model**: device-local `VkBuffer` allocated through
  [VMA](https://gpuopen.com/vulkan-memory-allocator/) (AMD's VMA
  library, MIT). Zero-copy import from FFmpeg hw-frames uses
  `VK_KHR_external_memory_fd` on Linux (DMABUF) and
  `VK_KHR_external_memory_win32` on Windows. On Apple Silicon via
  MoltenVK, external memory is not supported and we fall back to a
  host-staged copy — documented in the backend ADR-supplementary doc.
- **Pathfinder feature**: the first feature ported is **VIF**. VIF
  is the VMAF-critical hot path (dominates VMAF end-to-end cost);
  porting it first validates the runtime, the queue / fence model,
  the shader compile pipeline, and the DMABUF import path together.
  PSNR / SSIM follow once VIF is bit-close to scalar on two hardware
  vendors.
- **Meson flag**: `-Denable_vulkan=false` default. Symmetric with the
  other GPU flags. CI adds one vulkan leg (Mesa's
  [llvmpipe software renderer](https://docs.mesa3d.org/drivers/llvmpipe.html)
  or [lavapipe](https://docs.mesa3d.org/drivers/llvmpipe.html) on
  the Linux runners, since we don't yet have Vulkan hardware in
  GHA). Hardware validation happens on the developer boxes until a
  Vulkan-capable self-hosted runner exists.
- **Correctness contract**: Vulkan outputs must be within the
  existing GPU-backend tolerance band against the CPU reference
  (see [CLAUDE.md §8](../../CLAUDE.md) — GPU backends are NOT
  bit-identical to the CPU golden gate; they are tolerance-bounded).
  Enforced via the `cross-backend-diff` skill.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Vulkan compute (chosen) | Cross-vendor (NVIDIA/AMD/Intel), cross-OS incl. macOS via MoltenVK and mobile; present in every consumer graphics driver; solid DMABUF/Win32-handle import story | Lowest-level GPU API we'll maintain; hand-written queue + descriptor + fence management; SPIR-V toolchain in CI | This is the only option that covers macOS + mobile + consumer-Windows in one shader set |
| Metal | Native Apple Silicon perf; first-class toolchain on macOS | Apple-only (macOS / iOS); requires a separate MSL shader set maintained in parallel with whatever else ships; obj-c / Swift bridging | Would need to ship alongside (not instead of) Vulkan, doubling GPU-kernel maintenance cost |
| WebGPU / wgpu-native | Cross-browser and native via wgpu-native; modern API | Immature native story; ecosystem is in flux; WGSL shader language is third thing to maintain; Chrome-GPU-thread model doesn't fit our library pattern | Fine for a future web-scoring demo; wrong tool for the main backend story |
| Extend SYCL to cover the gaps | Reuse existing SYCL kernels; no new shader language | SYCL on macOS is a non-starter (no runtime); SYCL on consumer AMD/Intel Windows remains a packaging nightmare; mobile SYCL is essentially absent | Doesn't close any of the three stated gaps |
| OpenCL | Mature, simpler than Vulkan | Apple deprecated it in 2018; NVIDIA ships 1.2-only outside CUDA; modern SPIR-V tooling has moved to Vulkan | Trajectory is downward; not where the ecosystem is heading |
| Do nothing | Zero cost | The macOS / mobile / consumer-Windows-without-SDK gap stays open; user demand from issue #66 stays unmet | Chosen against — the demand exists and the cost is scoped |

## Consequences

**Positive**

- Closes macOS / mobile / consumer-Windows-without-SDK GPU gaps in
  one workstream.
- Gives the fork a compelling "runs on any GPU shipped since 2017"
  line for documentation.
- Provides a second vendor-neutral compute path (after SYCL),
  which is useful risk-hedging if the SYCL/oneAPI project pivots.
- The SPIR-V + GLSL pipeline is a well-documented, well-tooled
  industry standard — the skills are broadly transferable.

**Negative**

- Meaningful new code surface: runtime (~1500 LOC), plus per-feature
  kernels (~300–800 LOC each). Offset by the `add-gpu-backend`
  scaffold doing the first pass.
- Third GPU memory-model to debug. DMABUF import semantics differ
  between Vulkan-native and our existing CUDA/SYCL plumbing.
  Mitigated by writing the Vulkan DMABUF path against the same
  `VkExternalMemoryHandleType` our SYCL runtime already imports.
- SPIR-V + shaderc / glslc added to the build matrix. Handled by
  vendoring glslang / shaderc as a `subprojects/` wrap, or by
  making the Vulkan leg require a system-installed SDK — to be
  decided in the implementation PR.
- CI: no Vulkan hardware in GitHub Actions. We validate via lavapipe
  for now, which catches shader compilation + runtime API use but
  not hardware-dependent numerical accuracy. Hardware validation
  happens on developer machines until a self-hosted runner exists.

**Neutral**

- No impact on the Netflix CPU golden gate. Vulkan joins
  CUDA/SYCL/HIP as a "close to CPU within tolerance" backend, not a
  bit-identical one.
- No change to the public C ABI. `libvmaf_vulkan.h` is added
  symmetrically with the existing backend headers.

## References

- [req] AskUserQuestion popup answered 2026-04-20: "Vulkan compute
  backend — what's the stance?" → "Add it (scaffold)". Second popup:
  "Vulkan backend — first feature to port as the pathfinder?" →
  "VIF (most VMAF-critical)".
- [Research-0004](../research/0004-vulkan-backend-design.md) —
  design digest: SPIR-V toolchain, volk vs vulkan.h, VMA, DMABUF
  import options.
- [Khronos Vulkan 1.3 spec](https://registry.khronos.org/vulkan/specs/1.3/html/)
- [VK_KHR_external_memory_fd](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#VK_KHR_external_memory_fd)
- [ADR-0103](0103-sycl-d3d11-surface-import.md) — precedent for
  external-handle import in a GPU backend.
- [ADR-0122](0122-cuda-gencode-coverage-and-init-hardening.md) —
  precedent for dlopen-based loader + actionable init errors.
- [add-gpu-backend skill](../../.claude/skills/add-gpu-backend/SKILL.md)
- [CLAUDE.md §8](../../CLAUDE.md) — golden-gate tolerance rule for
  GPU backends.

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- Public header `core/include/libvmaf/libvmaf_vulkan.h` — present.
- Backend runtime tree `core/src/vulkan/` — present (common.c,
  dispatch_strategy.{c,h}, picture_vulkan.{c,h}, kernel_template.h,
  import.c, import_picture.h, AGENTS.md, meson.build).
- `enable_vulkan` Meson option declared and live.
- ADR-0175 (Accepted) shipped the audit-first scaffold; ADR-0186
  (Accepted) shipped the VkImage zero-copy import; ADR-0251 (this
  sweep) shipped the async pending-fence v2 model.
- Verification command:
  `ls core/include/libvmaf/libvmaf_vulkan.h core/src/vulkan/`.

### Status update 2026-05-09: MoltenVK validation lane added

Added an advisory CI lane on `macos-latest` (Apple Silicon) that
validates the **MoltenVK passthrough route** for the existing Vulkan
backend on macOS, complementary to the planned native Metal backend.
Per ADR-0028 immutability the body above is preserved verbatim; this
appendix records the operational follow-through:

- New CI job `Build — macOS Vulkan via MoltenVK (advisory)` in
  [`libvmaf-build-matrix.yml`](../../.github/workflows/libvmaf-build-matrix.yml)
  installs `molten-vk` + `vulkan-loader` + `vulkan-headers` +
  `shaderc` via Homebrew, points the loader at MoltenVK via
  `VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json`,
  and runs the three Vulkan smoke binaries
  (`test_vulkan_smoke`, `test_vulkan_pic_preallocation`,
  `test_vulkan_async_pending_fence`) against the Apple GPU.
- Lane is `continue-on-error: true` (advisory) until one green run
  on `master`. Not a required check; not in
  `required-aggregator.yml`.
- Operator-facing documentation lives at
  [`docs/backends/vulkan/moltenvk.md`](../backends/vulkan/moltenvk.md)
  with the install matrix, known-limitations table, and
  failure-mode playbook.
- See [ADR-0338](0338-macos-vulkan-via-moltenvk-lane.md) for the
  full decision record.
