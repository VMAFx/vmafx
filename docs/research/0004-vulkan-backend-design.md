<!-- markdownlint-disable MD013 MD060 -->
# Research-0004: Vulkan compute backend — toolchain, loader, memory model, DMABUF import

- **Status**: Active
- **Workstream**: [ADR-0127](../adr/0127-vulkan-compute-backend.md)
- **Last updated**: 2026-04-20

## Question

For a Vulkan compute backend that lives alongside CUDA / SYCL / HIP
under `core/src/vulkan/` and `core/src/feature/vulkan/`, what
are the concrete tool-chain and memory-model choices? Specifically:

1. Which Vulkan loader do we link (`vulkan.h` + `libvulkan.so` vs
   [volk](https://github.com/zeux/volk))?
2. Which shader language + compiler do we standardise on?
3. Which memory allocator (hand-rolled vs VMA)?
4. How do we import FFmpeg-decoded hw-frames without host round-trip?

## Sources

- Existing backend scaffolding:
  - [`core/src/cuda/`](../../core/src/cuda/) — dlopen-based
    loader pattern for `libcuda.so.1`.
  - [`core/src/sycl/`](../../core/src/sycl/) — USM picture pool
    and D3D11 external-handle import ([ADR-0101](../adr/0101-sycl-usm-picture-pool.md),
    [ADR-0103](../adr/0103-sycl-d3d11-surface-import.md)).
- [Khronos Vulkan 1.3 spec](https://registry.khronos.org/vulkan/specs/1.3/html/)
  — the normative reference.
- [volk](https://github.com/zeux/volk) — single-header meta-loader
  by Arseny Kapoulkine; MIT; currently ~5000 installs/week via vcpkg
  Conan.
- [VMA (Vulkan Memory Allocator)](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)
  — AMD; MIT; the industry-standard allocator; 23k GitHub stars.
- [shaderc](https://github.com/google/shaderc) — Google's
  glslang wrapper; provides `glslc` as the canonical GLSL → SPIR-V
  compiler.
- FFmpeg hwcontext docs:
  [libavutil/hwcontext_vulkan.c](https://ffmpeg.org/doxygen/trunk/hwcontext__vulkan_8c.html)
  — FFmpeg's own Vulkan hwcontext, useful for aligning the
  external-memory handle types.
- Mesa lavapipe docs — CPU-backed Vulkan implementation for CI.

## Findings

### 1. Loader: volk vs raw `vulkan.h`

Raw `vulkan.h` + link-time `-lvulkan` is the obvious choice but has
one deal-breaker: if the user's system doesn't have a Vulkan loader
(rare on Linux, common on headless containers and some macOS
setups), the whole `libvmaf.so` fails to load. The CUDA backend
solved the same problem for `libcuda.so.1` via a dlopen shim
(ADR-0122 context).

**volk** is a 2-file header-only meta-loader that:

- Calls `dlopen("libvulkan.so.1")` / `LoadLibrary("vulkan-1.dll")`
  at `volkInitialize()`.
- Loads instance- and device-level function pointers into globals
  without any build-time dependency on the Vulkan SDK.
- Plays correctly with shaderc / VMA / other Vulkan-ecosystem libs.
- Zero link-time Vulkan requirement; the package manifests just
  need the Vulkan SDK _headers_.

Decision: volk. Identical pattern to the existing dlopen'd CUDA
loader, no build-machine needs Vulkan pre-installed.

### 2. Shader language + compiler

Three viable options:

| Shader language | Compiler | Pros | Cons |
|---|---|---|---|
| **GLSL 4.60** | glslc (shaderc) | Most mature tooling; every Linux distro ships it; matches FFmpeg's own Vulkan shader style; SPIR-V 1.3 target fits Vulkan 1.3 baseline | C-era syntax; new shaders require remembering how GLSL handles unsigned math |
| HLSL | DXC (Microsoft) | Sharper tooling on Windows; larger user base from gamedev; nicer generics | Still foreign on Linux; HLSL → SPIR-V path is newer + less battle-tested for compute |
| Slang | slangc | Modern language design; first-class SPIR-V target | Too new; we'd be the only dependent in the libvmaf sphere |

GLSL wins on tooling maturity alone. Compiled to SPIR-V 1.3 offline
at build time via `glslc --target-env=vulkan1.3`; resulting `.spv`
files are embedded as byte arrays in the backend source tree
(not shipped as loose files — keeps the `install` set clean and
avoids runtime file-IO).

### 3. Memory allocator: VMA vs hand-rolled

Hand-rolled: possible, but Vulkan memory management is famously the
most error-prone surface of the API. Memory types, heap budgets,
alignment requirements, sub-allocation — all non-trivial.

VMA solves these once for the entire Vulkan ecosystem. Used by
FFmpeg's Vulkan hwcontext, Dolphin, RPCS3, Godot, and ~half the
Vulkan-based projects with serious production deployments. Single
header + .cpp; MIT; no external deps.

Decision: vendor VMA under `subprojects/vulkan-memory-allocator/`
(as a Meson wrap) and use it for all buffer/image allocation. Keeps
our code focused on the VMAF math, not memory plumbing.

### 4. DMABUF / external-memory import

Our existing SYCL and CUDA backends import FFmpeg-decoded frames
via `VK_KHR_external_memory_fd`-equivalents (CUDA has
`cuMemImportFromShareableHandle`; SYCL has the oneAPI external-memory
extension). Vulkan uses the most standards-y path:

- **Linux**: `VK_KHR_external_memory_fd` with
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`. FFmpeg
  `AV_HWDEVICE_TYPE_VULKAN` or `AV_HWDEVICE_TYPE_VAAPI` both expose
  the DMABUF fd we hand to `vkImportSemaphoreFdKHR` /
  `VkImportMemoryFdInfoKHR`.
- **Windows**: `VK_KHR_external_memory_win32` with
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` when source is
  `AV_HWDEVICE_TYPE_D3D11VA`.
- **macOS (MoltenVK)**: no external-memory support. Fall back to a
  host-staged copy (same path the SYCL backend uses for setups
  without USM page-migration support).

Timeline fencing uses `VK_KHR_timeline_semaphore` (Vulkan 1.2 core,
1.3 guaranteed). Matches FFmpeg's own hwcontext_vulkan semaphore
model.

### 5. CI strategy — lavapipe

GitHub Actions has no Vulkan hardware. Mesa lavapipe (the CPU
Vulkan driver) is enough to:

- Validate SPIR-V shaders compile and load.
- Validate VkInstance/VkDevice creation and queue lifecycle.
- Run the VIF kernel end-to-end and compare results against the
  CPU reference within a **looser** tolerance than hardware would
  produce (llvmpipe's math is not IEEE-strict).

What lavapipe does **not** cover:

- Real hardware-specific numerical behaviour (fp16/fp32 blending,
  tile-memory paths on Mali/Adreno).
- Real DMABUF import (no external-memory extensions exposed).

Proposal: lavapipe leg is a "did it compile / does it run" gate only.
Hardware-tolerance assertions run on developer machines or a
self-hosted runner. The gate-vs-soak distinction mirrors the
existing windows-GPU-build-only-legs policy
([ADR-0121](../adr/0121-windows-gpu-build-only-legs.md)).

## Open questions (for follow-up iterations)

- **shaderc packaging**: vendor as a `subprojects/` wrap, or require
  system-installed via `pkg-config shaderc`? Wrap is simpler for
  first-time contributors; system-installed is leaner for distros.
  Decide in the implementation PR once we have a concrete build
  time for the wrap.
- **First vendor to hardware-validate on**: NVIDIA or AMD first?
  NVIDIA has the most mature Vulkan + external-memory + DMABUF
  support on Linux; AMD consumer cards are the highest-demand
  Vulkan-only audience. Probably NVIDIA first (re-uses the CUDA
  box), AMD second.
- **Does VIF-in-Vulkan benefit from subgroup operations?** The VIF
  pyramid reductions are natural fits for
  `VK_KHR_shader_subgroup_uniform_control_flow` + subgroup
  intrinsics. Worth a profiling round before the AVX-512 reference
  becomes the speed ceiling.
- **Do we expose `libvmaf_vulkan.h` publicly?** Yes, symmetric with
  `libvmaf_cuda.h` / `libvmaf_sycl.h`. Function names start
  `vmaf_vulkan_*`.

## Next steps

1. Governance PR (this one) lands — opens the road for
   `/add-gpu-backend vulkan` to scaffold the tree.
2. Scaffold PR follows — runtime init, device selection, queue
   setup, empty feature kernel file. No VIF math yet.
3. VIF Vulkan kernel PR — port the existing scalar VIF as a set of
   compute shaders, one shader per VIF pyramid scale. Validate
   against scalar on a developer box.
4. DMABUF import PR — wire FFmpeg `AV_HWDEVICE_TYPE_VULKAN` frames
   into the Vulkan VIF path without host round-trip.
5. PSNR / SSIM Vulkan ports — follow the VIF pattern.
6. Hardware self-hosted runner investigation — separate workstream.
