<!-- markdownlint-disable MD013 MD060 -->
# ADR-0361: Metal compute backend — scaffold-only audit-first PR (T8-1)

- **Status**: Accepted
- **Status update 2026-05-15**: scaffold implemented (T8-1 complete);
  `core/include/libvmaf/libvmaf_metal.h` and `core/src/metal/`
  tree present on master; `-ENOSYS` stubs in place.
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: gpu, metal, apple-silicon, scaffold, audit-first, fork-local

## Context

The fork's GPU portfolio currently covers NVIDIA (CUDA), Intel (SYCL /
oneAPI), AMD (HIP / ROCm — scaffold + eight kernel-template consumers
per [ADR-0212](0212-hip-backend-scaffold.md)), and software /
cross-vendor (Vulkan compute) compute paths. The matrix has one
remaining first-class gap: Apple Silicon. The fork ships VideoToolbox
encoder integration plus NEON SIMD on Apple Silicon today (per
[ADR-0145](0145-motion-v2-neon-bitexact.md) and the wider NEON twin
story), but no GPU compute backend for libvmaf feature extraction.

Apple Silicon (M1+) is architecturally distinct from the discrete-GPU
backends already covered:

- **Unified memory** — host and device share the same physical memory
  with cache coherence; `MTLBuffer` allocations created with
  `MTLResourceStorageModeShared` are zero-copy across CPU↔GPU. This
  removes the H2D / D2H copy machinery the CUDA / HIP / Vulkan
  backends spend the bulk of their submit-side complexity on.
- **No PCIe** — there is no separable device memory pool; the GPU
  reads and writes the same DRAM the NEON CPU path does.
- **First-party Apple compute API** — Metal is the supported user-space
  surface; OpenCL is deprecated since macOS 10.14 and Vulkan on Apple
  reaches the GPU only through MoltenVK's translation layer (Vulkan
  → Metal command-buffer rewrite), which adds a second dependency
  edge plus measurable per-dispatch overhead.

Backlog item **T8-1** queues this work behind the four landed backend
families. The Vulkan T5-1 → T5-1b → T5-1c sequence and the HIP T7-10 →
T7-10b sequence have validated the audit-first split end-to-end (per
[ADR-0175](0175-vulkan-backend-scaffold.md),
[ADR-0212](0212-hip-backend-scaffold.md)): land static surfaces in one
focused PR, then runtime + kernels in follow-up PRs against a stable
base. T8-1 reproduces that pattern for Metal.

This ADR is the audit-first companion. Same shape as ADR-0212 for HIP,
ADR-0175 for Vulkan: ship the **static surfaces** (header, build
wiring, kernel stubs, smoke, docs) in a focused PR so the runtime PRs
that follow have a stable base to land on.

## Decision

### Land scaffold only — no Metal SDK linkage yet

The PR creates:

- Public header
  [`core/include/libvmaf/libvmaf_metal.h`](../../core/include/libvmaf/libvmaf_metal.h):
  declares `VmafMetalState`, `VmafMetalConfiguration`,
  `vmaf_metal_state_init` / `_import_state` / `_state_free`,
  `vmaf_metal_list_devices`, `vmaf_metal_available`. Mirrors the
  CUDA + Vulkan + HIP + SYCL pattern.
- Backend tree under
  [`core/src/metal/`](../../core/src/metal/) — `common.{c,h}`,
  `picture_metal.{c,h}`, `dispatch_strategy.{c,h}`,
  `kernel_template.{c,h}`, `meson.build`. Every entry point returns
  `-ENOSYS` or do-nothing.
- First feature kernel scaffold at
  [`core/src/feature/metal/integer_motion_v2_metal.c`](../../core/src/feature/metal/integer_motion_v2_metal.c)
  — registers `vmaf_fex_integer_motion_v2_metal` so callers asking
  by name resolve to a clean `-ENOSYS` from `init()`, mirroring the
  HIP sixth consumer (ADR-0267). The Objective-C / Metal Shading
  Language source files (`.m`, `.metal`) arrive with the runtime PR
  (T8-1b).
- New `enable_metal` feature option in
  [`core/meson_options.txt`](../../core/meson_options.txt),
  defaulting to **`auto`**: probes for `Metal.framework` /
  `MetalKit.framework` on macOS hosts, disabled elsewhere.
- Conditional `subdir('metal')` in
  [`core/src/meson.build`](../../core/src/meson.build);
  `metal_sources` + `metal_deps` threaded through
  `libvmaf_feature_static_lib` alongside the existing CUDA / SYCL /
  Vulkan / HIP / DNN aggregations.
- Smoke test
  [`core/test/test_metal_smoke.c`](../../core/test/test_metal_smoke.c)
  pinning the `-ENOSYS` contract for every public C-API entry
  point, plus the kernel-template helpers and the
  `motion_v2_metal` extractor registration (mirrors
  `test_hip_smoke.c`).
- New CI matrix row `Build — macOS Metal (T8-1 scaffold)` in
  [`libvmaf-build-matrix.yml`](../../.github/workflows/libvmaf-build-matrix.yml)
  that compiles on `macos-latest` with `-Denable_metal=enabled`.
  GitHub-hosted `macos-latest` runners ship the Metal SDK as part of
  the system framework set (`Metal.framework` lives at
  `/System/Library/Frameworks/Metal.framework`); no extra install
  step is required.
- New docs at
  [`docs/backends/metal/index.md`](../backends/metal/index.md) plus
  the index row in
  [`docs/backends/index.md`](../backends/index.md) flipped from
  "planned" to "scaffold only".

### Default `enable_metal` to `auto`, type `feature`

Three GPU-backend opt-in conventions exist on the fork today:

| Backend | Option type | Default | Reasoning |
|---|---|---|---|
| `enable_cuda` | `boolean` | `false` | NVIDIA-specific; needs explicit nvcc / CUDA SDK |
| `enable_sycl` | `boolean` | `false` | Intel-specific; needs icpx / oneAPI |
| `enable_hip` | `boolean` | `false` | AMD-specific; needs ROCm SDK at runtime |
| `enable_vulkan` | `feature` | `disabled` | cross-vendor; opt-in until kernel matrix complete |
| `enable_dnn` | `feature` | `auto` | available on every host that ships ONNX Runtime |

Metal's auto-probe is closer to `enable_dnn`'s shape than to the
GPU-vendor-pair triad's: every macOS 11+ host has the framework, no
extra install step is needed, and the host check is cheap (`host_machine.system() == 'darwin'`
in meson). Choosing `feature` / `auto` lets stock macOS dev builds
pick Metal up automatically the moment the runtime PR lands; Linux /
Windows builds see the auto-probe fail silently and `enable_metal`
resolves to disabled. This avoids the "AMD GPU on a stock Ubuntu CI
runner" silent-flip risk that pushed `enable_hip` to boolean-false.
The `enabled` value forces the Metal frameworks to be linked even on
non-macOS hosts (will fail; useful for CI verification of the macOS
lane shape).

### Apple Silicon-only (Apple GPU Family 7+); reject Intel-Mac

The runtime PR (T8-1b) will target Apple Silicon Macs (M1 and later,
GPU Family Apple 7+) only. Intel Macs are out of scope for two
reasons:

1. **Apple has discontinued Intel-Mac GPU paths**. The last Intel-Mac
   shipped in 2022; macOS 15+ no longer guarantees feature parity on
   Intel discrete GPUs. The fork targets currently-supported hardware.
2. **The unified-memory zero-copy story does not apply on Intel
   Macs**. The Metal abstraction is the same, but Intel-Mac discrete
   GPUs (Radeon Pro / Vega) sit behind PCIe; the runtime PR's submit
   path would have to re-introduce the H2D / D2H staging the unified-
   memory design eliminates. That's a 2× implementation cost for a
   shrinking platform.

The runtime PR will gate device selection on
`MTLGPUFamily.Apple7` (M1 and later) via
`-[id<MTLDevice> supportsFamily:]`. Intel Macs surface as `-ENODEV`,
matching the same posture the CUDA backend uses for non-Pascal cards.

### MetalCpp wrapper for the runtime layer

The runtime PR (T8-1b) will use Apple's official MetalCpp headers
(`<Metal/Metal.hpp>`, `<MetalKit/MetalKit.hpp>`) for the runtime
layer rather than Objective-C `<Metal/Metal.h>` or Swift. MetalCpp
is a single-header, header-only C++ wrapper that exposes the Metal
API as `NS::*` / `MTL::*` C++ classes with reference-counted
`NS::Object` lifetimes. Apple ships and supports it as the
recommended C++ binding.

Reference: <https://developer.apple.com/metal/cpp/> (accessed
2026-05-09).

This keeps the fork's runtime tree in C++ throughout (matches CUDA
`.cu` / SYCL `.cpp` / Vulkan `.cpp` precedent) and avoids dragging
Objective-C runtime dependencies into the libvmaf TUs that would
otherwise have to be `.mm` files.

The kernel sources themselves are written in Metal Shading Language
(`.metal`) and compiled to `.air` / `.metallib` archives via
`xcrun metal` at build time — the runtime PR ships the metallib
loader.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Native Metal (chosen) | Zero-copy unified memory; matches Apple's first-party compute API; no translation overhead; one dependency edge | Apple-platform-only; new build-system surface (Xcode toolchain probing); Apple GPU Family 7+ gating cuts off Intel Macs | Apple Silicon is the perf story for Apple-platform users; native Metal is the only path that exploits unified memory directly. The Intel-Mac drop is acceptable per the discontinuation reasoning above |
| MoltenVK passthrough (rejected) | Reuse the existing Vulkan backend verbatim; zero new code on the libvmaf side | Two dependencies (Vulkan loader + MoltenVK) instead of one; per-dispatch translation overhead (Vulkan command buffer → Metal command buffer rewrite) measurable on tight loops; MoltenVK's coverage of compute-shader features lags discrete-GPU drivers | Reject — the perf cliff the unified-memory story aims to win is exactly what MoltenVK pays back to the translation layer. MoltenVK is fine for graphics workloads; for compute it adds latency that defeats the Apple Silicon advantage |
| Intel oneAPI / SYCL on macOS (rejected) | Reuse the existing SYCL backend; one tooling surface across Intel CPUs / GPUs / iGPUs | SYCL's Apple-platform support is third-party (Codeplay) and has historically lagged the upstream icpx releases; oneAPI does not publish a macOS distribution; the Apple Silicon CPU-fallback path runs on host code, not GPU | Reject — the SYCL stack has no first-party path to the Apple Silicon GPU. The runtime would either fall back to CPU (already covered by NEON) or attempt MoltenVK-equivalent translation through OpenCL, which is deprecated |
| OpenCL on macOS (rejected) | First-party Apple support historically; portable | Deprecated by Apple since macOS 10.14 (2018); receives no driver updates; `cl_khr_subgroups` and modern compute extensions never landed on Apple's implementation | Reject — Apple's deprecation is final; building a new backend on an unsupported API is a one-release dead-end |
| Swift instead of MetalCpp for the runtime | Native to Apple's tooling; tighter integration with Swift Package Manager | Pulls a Swift compiler into the libvmaf build; the rest of the libvmaf C++ codebase has no Swift; ABI-bridging across Swift / C / C++ adds complexity | Reject — the fork's C++ codebase is the natural integration point; Apple ships MetalCpp specifically for C++ consumers |
| Objective-C `.m` / `.mm` for the runtime | Direct access to `<Metal/Metal.h>`; no extra wrapper layer | Pulls Objective-C runtime into the libvmaf TUs; mixes ARC with the existing C++ memory management; build-system has to teach meson about `.m` files | Reject — MetalCpp is the supported wrapper specifically because Apple does not want consumers writing Objective-C glue for compute workloads. Swift / Obj-C bridging is for app-layer code, not library-layer compute |
| Land scaffold + runtime + first kernel in one PR | Single round of review, the kernel is exercised against real Metal from the start | Too large; same review-bandwidth concern as ADR-0212 / ADR-0175; splits the trust boundary between "the scaffold compiles + smoke-tests on macOS CI" and "this kernel produces correct numbers" | Audit-first separation per the same pattern as ADR-0212 / ADR-0175 / ADR-0173 |
| Default `enable_metal` to `disabled` (boolean) | Matches `enable_cuda` / `enable_sycl` / `enable_hip` syntax | Forces every macOS dev to opt in explicitly even though the framework is universally available; pushes Metal further down the first-class-backend ladder than its actual deployment story warrants | Reject — Metal on macOS is the equivalent of "DNN on a host with ONNX Runtime installed"; auto-probing matches the deployment reality |
| Skip the first feature kernel scaffold (`integer_motion_v2_metal`) | Smaller initial PR | The HIP scaffold (ADR-0212) shipped without first-consumer kernel and the runtime PR (T7-10b) became correspondingly larger; the first-consumer scaffold lands cheaply (host-only, registration-only) and gives the runtime PR a stable consumer call site to flip | Include — first-consumer scaffold included in T8-1; the runtime PR (T8-1b) flips the kernel-template helper bodies, this consumer's call sites stay verbatim |

## Consequences

**Positive:**

- Header surface lands without committing to runtime details. Future
  Metal-targeting consumers (third-party tools, MCP surfaces) can
  compile against the API today; calls fail cleanly with `-ENOSYS`
  until the runtime arrives.
- Build matrix gains a new lane that compiles the scaffold every PR
  on `macos-latest` — bit-rot is caught immediately on the same
  hardware-class the runtime will eventually run on.
- The `/add-gpu-backend` skill is exercised on a fourth backend
  (after Vulkan and HIP); the scaffold serves as proof that the
  abstraction layer continues to scale.
- Apple Silicon users see a clear "this is the path forward" entry
  in `docs/backends/index.md` even before kernels exist, with a
  concrete `-Denable_metal=enabled` build flag.
- The first-consumer kernel scaffold (`motion_v2_metal`) reuses the
  HIP / CUDA twin pattern and lets the runtime PR's diff focus on
  body-flips rather than scaffold creation.

**Negative:**

- New build-system surface for Apple frameworks. The runtime PR will
  need to teach meson about `xcrun metal` for `.metal` shader
  compilation; the scaffold defers that complexity by shipping no
  `.metal` files yet.
- `vmaf_metal_available()` returns `1` when built with
  `-Denable_metal=enabled` regardless of whether the kernels are
  real. Same convention as Vulkan T5-1 / HIP T7-10; documented in
  the operator-facing doc.
- No FFmpeg patch in this PR. The fork's `ffmpeg-patches/` series
  doesn't currently consume the Metal API surface (no `metal_device`
  filter option, no `AVHWDeviceContext` Metal wiring); the runtime
  PR will add the filter option once `vmaf_metal_state_init`
  actually works. CLAUDE §12 r14 only requires patch updates when
  an existing patch already consumes the surface — `docs/rebase-notes.md`
  carries the T8-1 entry.
- One additional ENOSYS-stub family on the libvmaf surface. Acceptable
  per the audit-first precedent.

**Neutral / follow-ups:**

- Runtime PR (T8-1b) needs Apple Silicon CI bring-up. The
  `macos-latest` GitHub-hosted runner family includes both Intel
  (`macos-13`) and Apple Silicon (`macos-14`+) variants; the runtime
  PR will pin to an `arm64`-tagged runner so the smoke test
  exercises a real Apple GPU.
- T8-1c motion_v2 kernel PR — replaces the `kernel_template.c`
  bodies with real `MTLCommandQueue` / `MTLBuffer` /
  `dispatchThreadgroups` calls; ports the CUDA/HIP twin's algorithm
  shape verbatim.
- `enable_metal` default flip from `auto` to `enabled` happens once
  the kernel matrix proves bit-exactness against CPU — same posture
  as the `enable_vulkan` flip roadmap in ADR-0175 and the
  `enable_hip` follow-up in ADR-0212.

## Tests

- `core/test/test_metal_smoke.c` (sub-tests pin the scaffold
  contract):
  - `test_context_new_returns_zeroed_struct`
  - `test_context_new_rejects_null_out`
  - `test_context_destroy_null_is_noop`
  - `test_device_count_scaffold_returns_zero`
  - `test_available_reports_build_flag`
  - `test_state_init_returns_enosys`
  - `test_import_state_returns_enosys`
  - `test_state_free_null_is_noop`
  - `test_list_devices_returns_enosys`
  - `test_kernel_lifecycle_init_returns_enosys`
  - `test_kernel_buffer_alloc_returns_enosys`
  - `test_kernel_lifecycle_close_is_noop`
  - `test_kernel_buffer_free_is_noop`
  - `test_motion_v2_metal_extractor_registered`
- New CI lane: `Build — macOS Metal (T8-1 scaffold)` in the libvmaf
  build matrix. Compiles with `-Denable_metal=enabled` on
  `macos-latest` and runs the smoke test (the contract path is
  exercised even though the runtime is `-ENOSYS`).

## Verification gap (honest)

This PR ships compile-only plumbing. The Linux dev session that
authored it cannot run the macOS lane locally — `Metal.framework`
does not exist outside macOS hosts. The macOS CI lane is the
ground-truth gate. Reviewers verifying locally on a Mac can run:

```bash
meson setup build -Denable_metal=enabled
ninja -C build
meson test -C build test_metal_smoke
```

## What lands next (rough sequence)

1. **Runtime PR (T8-1b)**: `MTLCreateSystemDefaultDevice` /
   `id<MTLCommandQueue>` / `id<MTLBuffer>` lifecycle; `vmaf_metal_state_init`
   returns `0` on a real Apple Silicon device, `-ENODEV` on Intel
   Mac or non-Apple-Family-7 GPU. The smoke contract flips from
   "`-ENOSYS` everywhere" to "device_count >= 0, state_init succeeds
   when devices >= 1, skip when none". MetalCpp wrapper introduced.
2. **Motion v2 kernel PR (T8-1c)**: first feature on the Metal
   compute path. Bit-exact-vs-CPU validation via
   `/cross-backend-diff`. Mirrors the CUDA / HIP `motion_v2`
   reference algorithm verbatim.
3. **VIF + ADM + long-tail kernels (T8-1d…)**: parity with the CPU +
   CUDA + SYCL + Vulkan + HIP matrix.
4. **CI Apple Silicon runner pin** (post-runtime): pin the macOS lane
   to an `arm64`-tagged GitHub-hosted runner so the smoke test
   exercises a real Apple GPU rather than the Intel-Mac fallback.
5. **`enable_metal` default flip from `auto` to `enabled`**: only
   after the kernel matrix proves bit-exactness via the `places=4`
   cross-backend gate (mirrors the `enable_vulkan` and `enable_hip`
   roadmaps).

## References

- [ADR-0212](0212-hip-backend-scaffold.md) — HIP scaffold-only
  audit-first PR (T7-10). The most recent precedent this ADR mirrors.
- [ADR-0175](0175-vulkan-backend-scaffold.md) — Vulkan scaffold
  precedent. Both audit-first splits.
- [ADR-0127](0127-vulkan-compute-backend.md) — Vulkan runtime design
  (queue, buffer, dispatch model). Metal's `MTLDevice` /
  `MTLCommandQueue` / `MTLBuffer` API parallels Vulkan's queue +
  buffer model closely.
- [ADR-0145](0145-motion-v2-neon-bitexact.md) — NEON SIMD twin for
  motion_v2. Coordinates with Metal: NEON stays the CPU-side
  Apple-Silicon path; Metal is the GPU-side path. The two are
  complementary, not redundant.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — `places=4` cross-backend
  gate; the runtime PR's incoming numerics gate.
- [ADR-0246](0246-gpu-kernel-template.md) — GPU kernel-template
  decision; the source the Metal mirror tracks (via the HIP twin
  that mirrors the CUDA twin).
- [ADR-0028](0028-adr-maintenance-rule.md) — ADR maintenance rule
  this ADR follows.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — deep-dive
  deliverables checklist this PR ships.
- [ADR-0221](0221-changelog-adr-fragment-pattern.md) — changelog
  fragment pattern this PR follows.
- Apple Developer documentation — Metal-cpp,
  <https://developer.apple.com/metal/cpp/> (accessed 2026-05-09).
- `req` — user direction in T8-1 implementation prompt
  (paraphrased): "scaffold a Metal compute backend for libvmaf;
  comparable scope to ADR-0212 (HIP backend scaffold); produce the
  runtime + first feature kernel (motion_v2)". The runtime body
  itself is deferred to T8-1b per audit-first split; the
  first-feature kernel scaffold ships in this PR with a
  registration-only posture.
