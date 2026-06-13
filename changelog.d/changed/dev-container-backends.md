- **dev/container**: `vmaf-dev-mcp` now exposes every host GPU backend
  (CUDA + SYCL + Vulkan + HIP) on multi-GPU hosts. `dev/Containerfile`
  appends `${ONEAPI_ROOT}/tcm/latest/lib` to `LD_LIBRARY_PATH` so the
  level-zero UR adapter resolves `libhwloc.so.15` (was: SYCL "No
  platforms found" on Intel Arc even with device passthrough); drops
  the bogus `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`
  pin (was: Vulkan loader returned zero devices because the pinned
  file does not exist, hiding the NVIDIA Container Toolkit's
  bind-mounted `nvidia_icd.json` and mesa intel/radeon ICDs);
  `dev/scripts/dev-mcp-entrypoint.sh` `unset`s the VK ICD env vars at
  container start because Docker's `ENV` cannot truly unset and
  empty-string is treated as a missing ICD; adds a build-time backend
  probe that surfaces silent `built without X support` regressions in
  the image build log. `dev/docker-compose.yml` adds a read-only
  bind-mount of `/dev/dri/by-path` to both `dev-mcp` and
  `smoke-probe-cron` services so the udev pci symlinks the Intel
  compute-runtime relies on survive Docker's `devices:` translation.
- **libvmaf/tools** (`vmaf` CLI): `meson.build` now adds `-DHAVE_HIP=1`
  to `vmaf_tool_cflags` when `enable_hip` is true, mirroring the
  existing CUDA / SYCL / Vulkan conditionals. Without this define the
  `#ifdef HAVE_HIP` guards in `tools/vmaf.c` (HIP include, `VmafHipState`
  init/cleanup, `--backend hip` strict-mode arm) were compiled out even
  when libvmaf itself was built against HIP, and `vmaf --backend hip`
  returned the ADR-0498 "built without hip support" strict-mode error.
  ADR-0509 / Research-0138.
