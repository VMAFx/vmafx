- RC documentation cleanup — purged stale Vulkan references and corrected
  drifted facts across the root docs and the docs site, so the published
  documentation matches `master`:
  - **README.md**: fixed the license id to the valid SPDX
    `BSD-2-Clause-Patent` (badge + two footer lines; was the non-SPDX
    `BSD-3-Clause-Plus-Patent`, per ADR-1036); corrected the broken
    CPU build command to `meson setup build core ...` (build root is
    `core/` per ADR-0700); expanded the `--backend` list to
    `auto|cpu|cuda|sycl|hip|metal` and added the `--no_hip` /
    `--hip_device` / `--no_metal` / `--metal_device` flags; removed the
    broken "GCC 16 / ADR-0376" cross-reference (ADR-0376 is a Vulkan ADR,
    and the linked filename did not exist); corrected the HIP kernel count
    from "21 registered" to "19 registered + 3 unregistered stubs" (verified
    against `feature_extractor.c`).
  - **AGENTS.md**: replaced the live-Vulkan rebase invariants and dev-MCP
    treatment with ADR-0726 tombstones; corrected the container version pins
    to match `dev/Containerfile` (`cuda-toolkit-13-3`, unversioned
    `intel-basekit`, `ROCM_VER=7.2.4`); bumped the FFmpeg patch base from
    `n8.1` to `n8.1.1`.
  - **docs/metrics/** (`motion`, `features`, `ms-ssim`, `ssimulacra2`,
    `vif`): dropped Vulkan backend-availability rows / prose / dead
    `*_vulkan` source links and the `VMAF_VULKAN_DISPATCH` env-var; fixed
    the false "`float_ansnr_vulkan.c` remains in tree" claim.
  - **docs/backends/**: fixed the HIP kernel-count drift to 19+3
    (`hip/overview.md`, `index.md`, `cuda/overview.md`); repaired the broken
    `hip/index.md` link in `kernel-scaffolding.md`; reduced
    `vulkan/overview.md` to a runnable-command-free tombstone; removed the
    stale "Vulkan scaffold" cross-arch GPU claim from `arm/overview.md`.
  - **docs/architecture/phase4b-distributed-platform.md**: removed the live
    Vulkan-on-NVIDIA/-AMD/-Intel k8s GPU lanes.
