**BREAKING** — Vulkan compute backend removed (ADR-0726).

The Vulkan backend (`-Denable_vulkan=enabled`) and all associated CLI flags
(`--backend vulkan`, `--vulkan_device`, `--vulkan-require-fp64`, `--no_vulkan`)
have been removed. The `libvmaf_vulkan.h` public header is no longer installed.

Removed due to three long-standing unresolvable bugs (T-VK-1.4-BUMP,
T-VK-CIEDE-F32-F64, T-VK-VIF-1.4-RESIDUAL-ARC), no coverage gap after removal
(CUDA/HIP/SYCL/Metal cover all vendors in the k8s deployment model), and the
highest per-backend CI and maintenance footprint of any non-CUDA backend (~30 135
LOC removed).

**Migration**: Use `--backend cuda` (NVIDIA), `--backend hip` (AMD), `--backend
sycl` (Intel Arc), or `--backend metal` (Apple Silicon) as appropriate for your
hardware. All vendors retain native GPU coverage. CPU fallback remains the
universal default.

ABI note: `VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` (value 4) and
`VMAF_FEATURE_EXTRACTOR_VULKAN` (bit 5) are reserved numeric gaps — they are
not reused and the subsequent HIP enumerators are not renumbered.

See [ADR-0726](../docs/adr/0726-drop-vulkan-backend.md).
