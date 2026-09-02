<!-- markdownlint-disable MD013 MD028 MD060 -->
# Research Digest 0734: NVIDIA R610.43.02 Driver Changelog Audit

**Date:** 2026-05-28
**Branch:** `docs/r610-driver-changelog-audit-20260528`
**Driver installed locally:** 610.43.02 (loaded after reboot per user confirmation)
**Scope:** Assess driver-level changes affecting CUDA workloads (UVM, scheduler, power,
memory bandwidth); map findings to fork CUDA paths in `core/src/cuda/` and
`cmd/vmafx-server/`.

---

## 1. Changelog fetch status

| URL attempted | HTTP status |
|---|---|
| `https://download.nvidia.com/XFree86/Linux-x86_64/610.43.02/README/changelog.html` | **404 — not found** |
| `https://download.nvidia.com/XFree86/Linux-x86_64/610.43.02/README/` (directory index) | **200 — live** (no changelog.html linked) |
| `https://download.nvidia.com/XFree86/Linux-x86_64/610.43.02/README/index.html` | **200** (no changelog section) |
| `https://download.nvidia.com/XFree86/Linux-x86_64/610.43.02/README/newfeature.html` | **404 — not found** |
| `https://forums.developer.nvidia.com/t/linux-display-driver-beta-release-610-43-02/` | **404 — not found** |
| `https://forums.developer.nvidia.com/c/gpu-graphics/linux/148` (category index) | **200 — thread data extracted** |

**Conclusion:** NVIDIA has still not published a standalone `changelog.html` for the
610.43.02 series under the canonical XFree86 README path. The directory index at
`README/` is live but contains only the standard installation guide sections
(Introduction through Appendix L); no changelog entry point exists.

A substantive forum thread ("610 release feedback & discussion") was found via the
category index and yielded the content below. All quotes are sourced from that thread
as of 2026-05-28.

---

## 2. Confirmed R610.43.02 changes (from developer forum thread)

### 2.1 Vulkan extensions added

> "Added support for the following Vulkan extensions: VK_EXT_shader_long_vector
> VK_KHR_internally_synchronized_queues VK_NV_push_constant_bank"

> "Added support for creating Vulkan logical devices from multiple physical devices on
> select cards via VK_KHR_device_group_creation. This feature can be enabled by setting
> the environment variable `__VK_ENABLE_DEVICE_GROUPS=1`."

### 2.2 Display and framebuffer

- Added support for FP16 EGL framebuffer configurations on Wayland.
- Added support for DRM format modifiers for multiplanar YCbCr formats.
- Added support for mmap on DMABUF file descriptors exported from discrete NVIDIA GPUs.
- Added support in the nvidia-drm kernel module for the per-plane DRM color pipeline
  API introduced in Linux v6.19. A new `color_pipeline` kernel module parameter was
  added to `nvidia-drm` to allow disabling this support as a workaround.

### 2.3 Removals

- Removed support for using the NVIDIA X11 driver with Xinerama.

### 2.4 Bug fixes vs R580 series

- Fixed a Vulkan performance regression (vs upstream R580 series) that affected at least
  one shipping title.
- Fixed regressions introduced in 580.65.06 and 580.105.08.

### 2.5 GSP firmware

GSP firmware path: `/lib/firmware/nvidia/610.43.02/` (`gsp_*.bin`, architecture-split).
Enabled by default on Turing and later. No new GSP-level changes documented.

---

## 3. CUDA / UVM / scheduler findings

**None confirmed.** The forum thread, directory README, and all reachable 610.43.02
documentation are silent on:

- UVM (Unified Memory) subsystem changes
- CUDA kernel scheduler changes
- Concurrent multi-process execution (MPS) changes
- Power management or GPU clock policy changes specific to compute workloads
- Memory bandwidth or NVLINK changes
- New driver-level environment variables affecting CUDA (no new `NVIDIA_*` or
  `CUDA_*` knobs documented)

The known-issues section of the README covers only display/graphics topics; no
compute or UVM regressions are listed.

---

## 4. Impact mapping to fork code paths

### 4.1 `core/src/cuda/picture_cuda.c` — CUDA picture pool / pinned memory

No driver-level UVM or pinned-memory changes were found in R610.43.02. The async
`cuMemcpy2DAsync` pattern used for upload/download in `vmaf_cuda_picture_upload_async`
/ `vmaf_cuda_picture_download_async` is unaffected.

**Assessment:** No action required.

### 4.2 `core/src/feature/cuda/` — CUDA feature kernels / concurrent execution

No scheduler or concurrent-kernel-execution changes were found. The kernel concurrency
behavior on the RTX 4090 (Ada Lovelace, Turing-lineage GSP) is unchanged by this
driver drop.

**Assessment:** No action required.

### 4.3 `cmd/vmafx-server/` — gRPC server (multi-request concurrency)

No MPS, CUDA context, or process-isolation changes were documented. The server's
concurrency model (per-request CUDA context sharing via the fork's dispatch strategy
in `core/src/cuda/dispatch_strategy.c`) is unaffected.

**Assessment:** No action required.

### 4.4 DMABUF / DRM format modifier changes

The addition of mmap support on DMABUF FDs exported from discrete NVIDIA GPUs
(`Added support for mmap on DMABUF file descriptors`) is potentially relevant to the
fork's SYCL USM/dmabuf import path (`core/src/sycl/`), not CUDA directly. This is a
kernel-module improvement that widens what user-space can do with exported dmabuf
handles. No code changes are required now; if a zero-copy import path is added for the
CUDA backend in the future, the DMABUF mmap capability in R610 is a prerequisite that
is now confirmed satisfied.

**Assessment:** Informational; no immediate action.

---

## 5. Summary verdict

R610.43.02 is a display-focused release relative to the R580 series. The changes
that are confirmed (new Vulkan extensions, DRM color pipeline, FP16 EGL, DMABUF mmap,
Xinerama removal) are all in the graphics/Wayland/DRM layer and have no effect on
CUDA compute paths. The canonical `changelog.html` remains unpublished at NVIDIA's
XFree86 index. No driver-level UVM, scheduler, power-management, or CUDA-specific
changes were found that require code or configuration changes in the fork.

---

## 6. Reproducer

```bash
# Primary source (200 — live at time of research)
curl -sI 'https://download.nvidia.com/XFree86/Linux-x86_64/610.43.02/README/'

# Still 404 at time of research
curl -sI 'https://download.nvidia.com/XFree86/Linux-x86_64/610.43.02/README/changelog.html'

# Forum thread (200 — category index, thread "610 release feedback & discussion")
# https://forums.developer.nvidia.com/c/gpu-graphics/linux/148
```
