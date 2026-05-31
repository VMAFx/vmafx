# Research-0541: dev-mcp container full GPU plumbing — root cause and fix matrix

- **Status**: Active
- **Workstream**: ADR-0541
- **Last updated**: 2026-05-18

## Question

Why does the `vmaf-dev-mcp` container produce silent CPU fallback or
software emulation on three of the four GPU backends (Vulkan, SYCL,
HIP) even after ADR-0509 / ADR-0514 / ADR-0528 wired up the
bind-mounts and ICD packages, and what is the minimum surgical fix per
backend?

## Sources

- Vulkan-Loader env-var reference:
  <https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md>
  (canonical doc for `VK_DRIVER_FILES` allowlist semantics + ICD search
  order).
- NVIDIA Container Toolkit driver-capabilities reference:
  <https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html#driver-capabilities>
  (documents the `graphics` token's role in mounting
  `nvidia_icd.json`).
- ROCm 6.x supported-GPU list:
  <https://rocm.docs.amd.com/projects/install-on-linux/en/latest/reference/system-requirements.html>
  (confirms `gfx1036` is **not** on the supported-GPU allowlist;
  `gfx1030` / `gfx1100` / `gfx1101` are).
- Intel compute-runtime + VA-API dlopen chain:
  <https://github.com/intel/compute-runtime/blob/master/shared/source/os_interface/linux/drm_neo.cpp>
  (the `Drm::createDrm` path that probes `libva-drm.so.2` →
  `iHD_drv_video.so` during GPU enumeration).
- Prior fork ADRs: [ADR-0509](../adr/0509-vulkan-icd-env-contract.md),
  [ADR-0514](../adr/0514-dev-mcp-container-gpu-exposure.md),
  [ADR-0528](../adr/0528-dev-dri-whole-directory-bind-mount.md),
  [ADR-0530](../adr/0530-hip-feature-flag-promotion-and-picture-buffer.md),
  [ADR-0540](../adr/0540-dev-container-ffmpeg-av1-and-hwaccel-encoders.md).
- Empirical measurement on the dev machine (2026-05-18):
  - `vkinfo` inside the container reports two physical devices on a
    stock mesa + Container Toolkit image: `[0] lavapipe (Software)`,
    `[1] NVIDIA RTX 4090`. `vmaf --backend vulkan --vulkan_device 0`
    silently picks `[0]`.
  - `rocminfo` returns `hsaKmtOpenKFD ... Invalid argument` /
    `hsa_init() = HSA_STATUS_ERROR_OUT_OF_RESOURCES` against
    `gfx1036` without `HSA_OVERRIDE_GFX_VERSION`. Setting
    `HSA_OVERRIDE_GFX_VERSION=10.3.0` makes `hsa_init()` succeed and
    enumerates the iGPU as `gfx1030 (override)`.
  - `sycl-ls` reports `Platforms: 0` without
    `intel-media-va-driver-non-free`; reports
    `level_zero:gpu [Intel Arc A380]` once the package is installed.

## Findings

### Backend 1: Vulkan — lavapipe wins the `vulkan_device=0` race

The Vulkan loader enumerates ICDs in directory-walk order, which on
Ubuntu 24.04 + mesa 24.x produces:

```text
/usr/share/vulkan/icd.d/intel_icd.x86_64.json   (mesa, present on Intel hosts)
/usr/share/vulkan/icd.d/lvp_icd.x86_64.json     (mesa lavapipe, always present)
/usr/share/vulkan/icd.d/radeon_icd.x86_64.json  (mesa, present on AMD hosts)
/etc/vulkan/icd.d/nvidia_icd.json               (Container Toolkit bind-mount)
```

`vkEnumeratePhysicalDevices()` returns one `VkPhysicalDevice` per ICD
in the order each ICD's `vkEnumeratePhysicalDevices` reports. The
order is implementation-defined but mesa registers them in
filesystem-walk order; the Container Toolkit's
`/etc/vulkan/icd.d/nvidia_icd.json` is read *first* (`/etc/` is
searched before `/usr/share/`), so on an NVIDIA-only host the bind-
mount wins. But on a multi-vendor host that also has Intel + AMD via
mesa, `/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` lands at index 1
or 2 (after the NVIDIA bind-mount) and is what `--vulkan_device 0`
selects when the user explicitly enumerates physical devices.

The fix is the entrypoint-time `VK_DRIVER_FILES` rewrite: enumerate
every JSON under both standard ICD paths, drop anything matching
`lvp_*` / `lavapipe*`, and pin the loader to the resulting colon-
separated allowlist. This works on every Vulkan loader release that
supports `VK_DRIVER_FILES` (≥ 1.3.207; Ubuntu 24.04 ships 1.3.275).

### Backend 2: SYCL — Intel compute-runtime needs VA-API drivers

The Intel compute-runtime (`libze_intel_gpu.so.1`) dlopens
`libva-drm.so.2` at `zeInit()` time to query DRM render-node
capabilities. `libva-drm.so.2` is provided by `libva-drm2` (already
in the apt list), but the actual VA-API codec backend it loads is
`/usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so` (Intel) /
`radeonsi_drv_video.so` (AMD), both of which live in
`intel-media-va-driver-non-free` / `mesa-va-drivers` respectively.
Without them, `vaInitialize()` returns `VA_STATUS_ERROR_UNKNOWN`,
which the compute-runtime treats as "no GPU available" and bails out
of the L0 driver enumeration. `sycl-ls` then reports `Platforms: 0`
even though the kernel module + DRI nodes are visible.

### Backend 3: HIP — `gfx1036` blocked by ROCm 6.x allowlist

`libhsa-runtime64.so` reads `/sys/class/kfd/kfd/topology/nodes/*/gpu_id`

- `name` at `hsa_init()` time and rejects any device whose GFX IP
version is not on the hardcoded allowlist (see ROCm's
`HSAKMT_STATUS_NOT_SUPPORTED` path in
`rocm/rocminfo/rocm_smi_lib`). `gfx1036` (RDNA2 IP rev 10.3.6, the
Raphael iGPU shipped in AMD desktop CPUs with integrated graphics) is
not on the ROCm 6.x list; `gfx1030` (RDNA2 desktop dGPU) is.

The supported escape hatch is `HSA_OVERRIDE_GFX_VERSION=10.3.0`,
which makes the runtime treat the device as `gfx1030` for code-object
selection. This is documented in ROCm's troubleshooting guide for
the iGPU + APU lineup. The libvmaf HIP kernels do not use any
gfx1030-vs-gfx1036 divergent instructions (verified empirically in
ADR-0530 / ADR-0538 — the integer-VIF and motion-score kernels
produce VMAF scores within places=3 of the CPU baseline against the
override).

`HSA_ENABLE_SDMA=0` is a defensive setting: on RDNA2 iGPUs (sharing
system RAM with the CPU), the SDMA copy engine can trigger VM faults
on small device→host transfers. The libvmaf collect path is
dominated by such transfers (per-frame score readback); empirically
disabling SDMA on `gfx1036` eliminates one class of stall observed
during ADR-0538's HIP integer-VIF runs.

`ROCR_VISIBLE_DEVICES=0` pins HIP to the single AMD adapter on
multi-iGPU + dGPU hosts so kernels cannot accidentally dispatch onto
a non-RDNA2 device that needs a different `HSA_OVERRIDE_GFX_VERSION`.

### Backend 4: NVIDIA Vulkan — `NVIDIA_DRIVER_CAPABILITIES` invariant

The NVIDIA Container Toolkit only bind-mounts `nvidia_icd.json` into
`/etc/vulkan/icd.d/` when the `graphics` token is in
`NVIDIA_DRIVER_CAPABILITIES`. The default token set for a `runtime:
nvidia` container without `NVIDIA_DRIVER_CAPABILITIES` set is
`compute,utility` — which gets you CUDA + nvidia-smi but **not**
Vulkan. The fork's compose file already sets
`compute,graphics,utility,video`, but a stale operator habit of
trimming the env block ("we only need CUDA") would silently disable
NVIDIA Vulkan. The fix is documentation-only here.

## Alternatives explored

See ADR-0541's `## Alternatives considered` table for the rejected
options (delete lvp_icd.json from disk; use
`VK_LOADER_DRIVERS_DISABLE`; switch to ROCm 7; drop mesa-vulkan-
drivers entirely).

## Open questions

- ROCm 7 noble release timeline. Once available, the
  `HSA_OVERRIDE_GFX_VERSION` lie should be re-evaluated against the
  native `gfx1036` support that ROCm 7 reportedly adds.
- Whether libvmaf should grow a `--vulkan-no-software` CLI flag that
  enforces the same lavapipe filter at the library level (so the
  guarantee survives operators who unset `VK_DRIVER_FILES` per-
  invocation). Tracked as a follow-up; out of ADR-0541's scope.

## Related

- ADRs: [ADR-0509](../adr/0509-vulkan-icd-env-contract.md),
  [ADR-0514](../adr/0514-dev-mcp-container-gpu-exposure.md),
  [ADR-0528](../adr/0528-dev-dri-whole-directory-bind-mount.md),
  [ADR-0530](../adr/0530-hip-feature-flag-promotion-and-picture-buffer.md),
  [ADR-0540](../adr/0540-dev-container-ffmpeg-av1-and-hwaccel-encoders.md),
  [ADR-0542](../adr/0542-dev-container-full-gpu-plumbing.md).
- PRs: this PR (fix/dev-container-full-gpu-plumbing).
