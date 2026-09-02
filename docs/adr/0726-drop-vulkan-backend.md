<!-- markdownlint-disable MD060 -->
# ADR-0726: Drop Vulkan backend

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: Lusoris (user direction)
- **Tags**: vulkan, gpu, backend, build, breaking, fork-local

## Context

The VMAFX fork shipped a Vulkan compute backend (ADR-0127, T5-1/T5-1b) for
cross-vendor GPU portability. After the rebranding and the shift to a
container-first Kubernetes deployment model, the original rationale no longer
applies: each vendor runs in its own k8s node pool (`nvidia.com/gpu`,
`amd.com/gpu`, `gpu.intel.com/i915`) and the appropriate native backend
(CUDA, HIP, SYCL) is injected at pod-schedule time via `VMAFX_BACKEND`.
Vulkan provides no coverage that the native trio does not already provide on
the same hardware.

Three long-standing open bugs accumulated with no resolution path:

- **T-VK-1.4-BUMP** — NVIDIA FP-contraction regression at API 1.4 on driver
  595.71+. Blocked for months despite three shader fix phases (ADR-0264,
  ADR-0269).
- **T-VK-CIEDE-F32-F64** — Structural f32/f64 ciede2000 precision gap on
  NVIDIA. Accepted as documented debt (ADR-0273); not closeable without
  `shaderFloat64` (rejected).
- **T-VK-VIF-1.4-RESIDUAL-ARC** — vif residual mismatch on Intel Arc A380
  (Mesa-ANV / DG2) surviving Phase-3b stronger-fence experiment. Open since
  2026-05-08.

The Vulkan backend also had the highest per-backend CI footprint of any
non-CUDA backend (3 CI jobs, 2 build-matrix rows, 8 test binaries) and
the highest source LOC (~30 000 lines including 24 feature files, GLSL
shaders, VMA runtime, and volk integration).

Research-0733 (hardware backend audit, 2026-05-28) confirmed the drop
recommendation: no vendor loses native GPU coverage after the drop. The only
features exclusively on Vulkan and not on SYCL are `float_ssim` and `ssim`
float aliases — both are on CUDA and HIP, and `float_ssim` is a legacy
extractor unused by any production VMAF model. Intel users fall back to
CPU for that extractor, which is acceptable.

## Decision

Remove the Vulkan backend entirely from the VMAFX fork, including:

- `core/src/feature/vulkan/` (24 feature extractor source files + shaders)
- `core/src/vulkan/` (runtime: common, dispatch, picture pool, VMA, volk)
- `core/include/libvmaf/libvmaf_vulkan.h` (public API header)
- `core/test/test_vulkan_*.c` (8 test binaries)
- All Vulkan CI workflow rows (3 jobs in tests-and-quality-gates.yml, 2 rows
  in libvmaf-build-matrix.yml, 1 job in ffmpeg-integration.yml, fuzz.yml +
  sanitizers.yml cleanup)
- `ffmpeg-patches/0004-libvmaf-wire-vulkan-backend-selector.patch` and
  `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch` (CLAUDE §12
  r14 — updated in the same PR)
- `meson_options.txt`: `enable_vulkan` option removed
- CLI: `--no_vulkan`, `--vulkan_device`, `--vulkan-require-fp64` flags removed
- Container and Helm chart references cleaned up

ABI preservation: `VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` (enum value 4 in
`picture.h`) and `VMAF_FEATURE_EXTRACTOR_VULKAN` (flag bit 5 in
`feature_extractor.h`) are left as **reserved gaps** rather than renumbered,
to avoid silently shifting the numeric values of the HIP enumerator that
follows. Any future reuse of these slots requires an explicit ABI-bump ADR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep Vulkan | Preserves cross-vendor single-binary portability story | 3 unresolvable open bugs, highest CI footprint, 30 000 LOC to maintain, no k8s-native representation | The portability story is moot in the k8s model where each vendor has its own node pool and native backend |
| Drop only kernels, keep Vulkan runtime | Reduces LOC burden while preserving the framework for future reuse | Still requires maintaining the VMA/volk integration, CI slots, and public API surface; the 3 open bugs all require real GPU hardware to verify resolution | The maintenance cost is in the runtime + CI, not just the kernels |
| Defer until Phase X | No immediate disruption | The 3 bugs have no resolution timeline; deferring means carrying them indefinitely; Research-0733 recommends immediate action | The decision cost only grows; containers already deploy native backends |
| Drop only when Apple-Silicon vendor SDK matures | Would keep Vulkan as a stopgap for Apple | Apple Silicon already has Metal (ADR-0420, T8-1b); MoltenVK testing was advisory and never promoted to required | Metal is the correct path for Apple; MoltenVK was never load-bearing |

## Consequences

- **Positive**: −30 135 LOC, −3 open bugs, −3 CI jobs, −2 build matrix rows,
  −2 ffmpeg patches, reduced build system complexity, no k8s coverage gap.
- **Negative**: `float_ssim` extractor is no longer available on SYCL (Intel)
  GPU path; falls back to CPU. This is acceptable — `float_ssim` is a legacy
  float-pipeline metric not used in any production VMAF model.
- **Neutral / follow-ups**: lavapipe wiring in `dev/Containerfile` and
  `dev/docker-compose.yml` cleaned up. The volk and VMA meson wraps in
  `core/subprojects/` may be removed in a separate follow-up PR once no other
  consumer remains.

## References

- `req`: user direction — the Vulkan backend should be dropped; lavapipe
  wiring is not useful.
- [Research-0733](../research/0733-hardware-backend-audit-2026-05-28.md) —
  hardware backend audit recommending the drop.
- [ADR-0127](0127-vulkan-compute-backend.md) — original Vulkan backend design
  (superseded by this ADR).
- [ADR-0264](0264-vulkan-1-4-bump-blocked-on-fp-contraction.md) — T-VK-1.4-BUMP
- [ADR-0391](0391-ciede-vulkan-nvidia-f32-f64-precision-gap.md) — T-VK-CIEDE-F32-F64
- [ADR-0186](0186-vulkan-image-import-impl.md) — ffmpeg-patches r14 precedent
- [ADR-0860](0860-ffmpeg-patch-chain-no-op-vulkan-shim.md) — supplemental: the
  Vulkan FFmpeg patches (0004 + 0006) were retained in `series.txt` as no-op
  compatibility shims because downstream patches reference Vulkan-conditional
  context lines. The Vulkan runtime stays removed; the shims contribute zero
  linked code at FFmpeg configure time.
