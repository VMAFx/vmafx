# ADR-3402: Vulkan integer compute path (draft)

- **Status**: Draft (in-tree proposal for 1.1.0, issue [#3319](https://github.com/VMAFx/vmafx/issues/3319))
- **Date**: 2026-11-07
- **Deciders**: Lusoris (invitation to propose), luoye-cpu (implementation)
- **Supersedes**: [ADR-0726](0726-drop-vulkan-backend.md)
- **Tags**: vulkan, gpu, backend, integer, draft, fork-local

## Context

ADR-0726 removed the fork's first Vulkan backend with three arguments: the
k8s deployment model gives Vulkan no coverage the native trio lacks, three
unresolvable open bugs (T-VK-1.4-BUMP, T-VK-CIEDE-F32-F64,
T-VK-VIF-1.4-RESIDUAL-ARC), and the maintenance weight of ~30 000 LOC with
the largest CI footprint of any backend.

Issue #3319 re-opened the question with a narrower shape: not the removed
backend, but an **integer compute path** — the arithmetic that the SYCL and
Metal twins already share (ADR-0545 registry, ADR-1194's single angle
predicate, the fp64-free `adm_angle_flag_i64()` reformulation) — carried by
a deliberately minimal runtime. Three facts changed since the drop:

1. **The fp64 gap closed at the algorithm level.** The removed backend's
   structural ciede/f64 bugs all traced to requiring `shaderFloat64`.
   The fork's integer reformulations (`adm_angle_flag_i64()`,
   docs/research/2030) remove fp64 from the picture entirely: the draft
   shader needs `shaderInt64` only, which every Vulkan 1.1+ compute
   device reports — including Intel iGPUs whose drivers report
   `shaderFloat64 = 0` and where the SYCL `float_adm` twin cannot run.
2. **The runtime surface shrank by two orders of magnitude.** The removed
   backend carried volk, VMA, 24 feature files, import surfaces and CI
   rows. The draft path is a dynamic-loader function table (~48 entries),
   one compute queue, host-visible staging buffers, and one shader.
3. **The compile-time contract changed.** No SDK linkage at all: the
   runtime compiles with `VK_NO_PROTOTYPES` against headers only, loads
   `vulkan-1` / `libvulkan.so.1` at run time, and the GLSL shaders are
   compiled to SPIR-V at build time and embedded — the same shape the HIP
   backend already uses for HSACO embeds.

## Decision

Land a **draft Vulkan backend** with the following scope, behind
`-Denable_vulkan=true` (default off):

- **One feature**: `adm_vulkan`, the integer ADM twin — the 4-scale
  DWT/CSF/CM pipeline as one SPIR-V compute shader with the fp64-free
  angle predicate. The remaining default-model twins (motion, vif, ciede,
  ssim family, cambi) and the 10-bit path are known follow-ups, each one
  PR wide once the runtime is in-tree.
- **Runtime**: `core/src/vulkan/` — loader table, device bring-up
  (discrete before integrated, `device_index` override), buffers,
  per-extractor pipelines. No zero-copy import surfaces: pictures are
  plain HOST pictures the extractor uploads through host-visible memory,
  the same posture the HIP scaffold had before its runtime PR.
- **Public surface**: the four `vmaf_vulkan_*` entry points of
  `libvmaf_vulkan.h`, mirroring the HIP header minus the deprecation.
  Without `enable_vulkan`, every entry point returns `-ENOSYS` (engine
  stubs + compat stubs, the ADR-0519/ADR-1852 pattern).
- **One CI shape**: the backend compiles on every host (headers +
  glslang only); device-gated tests skip without a device, like the CUDA
  lane's.

### The k8s coverage argument, answered

ADR-0726 argued each vendor runs its own node pool with a native backend
injected at schedule time. That argument was about **coverage of features
on shipped hardware**. The draft path does not compete with the native
trio on CUDA/AMD/Intel-dGPU hardware — it covers the case the argument
itself created and left open:

- **Intel iGPU nodes** (`gpu.intel.com/i915` pools): the SYCL backend's
  `float_adm` twin needs `shaderFloat64` for its adm path (Intel drivers
  report it as absent on Raptor Lake iGPU — observed directly in the PoC
  round, where `vkCreateDevice` with the feature requested SIGSEGVs the
  driver twice instead of returning `VK_ERROR_FEATURE_NOT_PRESENT`). The
  fp64-free Vulkan path runs on exactly those devices with the CPU-exact
  integer arithmetic. The fork gains adm acceleration on the vendor whose
  native backend is the furthest from the CPU twins.
- **Mixed fleets / one-image portability**: a single libvmaf build that
  scores adm on any vendor's compute device removes the per-vendor image
  matrix for the integer path. ADR-0726 accepted that matrix as a cost of
  dropping Vulkan; the integer path buys it back for one feature family
  at ~2 500 LOC instead of ~30 000.
- **The vendor-neutral fallback lane**: where a pod gets no native
  backend (CPU-only pools with an iGPU present, or emulated/GPU-less CI
  consumers of the image), the Vulkan path is the only lane that can use
  the device the pod actually has. SYCL covers Intel iGPUs only where the
  DPC++ runtime ships; Vulkan's loader is present on every desktop and
  CI image with graphics drivers.

In short: ADR-0726's conclusion — *native backends for native vendor
pools* — stands, and this ADR does not reopen it. What changed is that
the *residual coverage gap* the drop accepted now has a one-feature,
fp64-free, SDK-free implementation whose maintenance profile is a shader
and a loader table, not a runtime.

### ABI: reclaiming the ADR-0726 reservations

ADR-0726 left three reserved gaps and required an explicit ABI-bump ADR
for reuse. This ADR is that ADR:

- `VMAF_BACKEND_VULKAN = 5` (libvmaf.h) — reused, now documented as
  `vmaf_vulkan_import_state()`.
- `VMAF_FEATURE_EXTRACTOR_VULKAN = 1 << 5` (feature_extractor.h) —
  reused, set on extractors under `feature/vulkan/`.
- `VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` — **stays reserved**. The
  draft reads HOST pictures only; the import story (AVVkFrame /
  external-memory, the RC4 WP3 lane's `VMAFX_MEMORY_VULKAN`) is future
  work with its own ADR. No picture.h change in this PR.

The libvmaf.so.3 compat surface carries the four functions via the
generated `[[compat]]` entries (`engine_with = "vulkan"`); the
`vmafx_legacy_vulkan.map` node follows the HIP one.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the status quo (no Vulkan) | Zero maintenance | Intel iGPU adm coverage stays on CPU; the single-image portability story stays closed | The draft's cost is one shader + a loader table; the gap it fills is the one ADR-0726 explicitly accepted |
| Grow SYCL to cover shaderFloat64-less devices with an integer adm twin | Reuses the SYCL runtime | The DPC++/AdaptiveCpp runtimes are heavier than a Vulkan loader on those nodes; an int64 SYCL kernel is the same algorithm anyway, and the CPU-exact path then exists in two frameworks instead of one shared predicate file | The Vulkan path shares `adm_angle_flag_i64()` like the SYCL/Metal twins already do; the delta is the runtime, and the Vulkan runtime is the smaller one for this device class |
| Re-adopt the removed backend wholesale | Everything lands at once | The three ADR-0726 bugs return; volk/VMA/import CI weight returns before any of it is load-bearing | The draft re-lands the floor first; every further feature is a reviewable increment |

## Consequences

- **Positive**: Intel iGPU (shaderFloat64=0) adm acceleration; single-image
  integer-path portability; the ABI slots ADR-0726 reserved are either
  reclaimed (two) or explicitly kept reserved with a reason (one); the
  `adm_angle_flag_i64()` predicate gains its third consumer.
- **Negative / follow-ups** (each PR-scoped, none blocking the draft):
  - Remaining default-model twins (motion, vif, ciede, ssim, cambi) and
    the 10-bit path; the option table beyond the default configuration.
  - Zero-copy picture import (`VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE`
    and the RC4 WP3 lane's Vulkan memory) — its own ADR.
  - A CI lane with a software rasterizer (lavapipe) for shader compile
    parity without hardware.
- **Neutral**: builds without `enable_vulkan` are unchanged except the
  four `-ENOSYS` compat symbols and one registry entry behind `#if`.

## Evidence

The draft's acceptance gate (the same one #3319's PoC round ran):

- Shader bit-exact against the CPU twin on RTX 5080 (sm_120), RTX 4060
  Laptop (sm_89) and Intel Raptor Lake-S iGPU (the fp64-free path works
  where shaderFloat64 is absent — the #3319 carried DEVICE_LOST item).
- 48-frame x 11-field hex-float exact on the 576x324 and 640x480 gate
  fixtures, plus a 7-geometry sweep (256x144 … 512x288) after the CM
  mirror-clamp fix (the CPU's `(i == 0) ? 1 : i - 1` edge tap asymmetry).
- In-tree `test_vulkan_adm_parity` (bit-exact debug keys, refusal cases
  for 10-bit and non-default options) passes on the 5080; both default
  builds (with and without `enable_vulkan`) compile clean; `vmaf
  --list-backends` reports the backend on both sides.
