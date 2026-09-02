# Research digest: dev-MCP container SYCL+HIP runtime UAPI pin (ADR-0541)

- **Date**: 2026-05-18
- **Author**: Claude (Anthropic), reviewed by lusoris
- **Scope**: Root-cause analysis of `vmaf --backend sycl|hip` silently
  falling back to CPU inside `vmaf-dev-mcp` on the dev host (CachyOS
  Linux 7.0.8, Intel Arc A380, AMD gfx1036, NVIDIA RTX 4090).

## Method

1. **Reproduce** with `docker exec vmaf-dev-mcp sycl-ls --verbose`
   and `docker exec vmaf-dev-mcp rocminfo`.
2. **Trace level-zero loader** with `ZE_ENABLE_LOADER_DEBUG_TRACE=1`
   and a direct C harness calling `zeInit(0)`.
3. **Trace Khronos OpenCL ICD** with `OCL_ICD_ENABLE_TRACE=1` against
   the system `libOpenCL.so.1`.
4. **Verify device passthrough** via `/sys/dev/char/`, `/dev/dri/by-path`,
   `cat /sys/class/drm/renderD129/device/{vendor,device}`.
5. **Compare to host state**: `pacman -Q intel-compute-runtime
   level-zero-loader hsa-rocr` (Arch host).

## Findings

### 1. SYCL: NEO compute-runtime version too old for kernel-7.x UAPI

The container shipped Intel NEO 25.18.33578.15 from
`https://repositories.intel.com/gpu/ubuntu noble unified` (the newest
version in that APT repo as of 2026-05-18). Host kernel is
`Linux 7.0.8-1-cachyos`; Arch host runs NEO 26.18.38308.1 and works.

Diagnostic flow:

```text
$ ZE_ENABLE_LOADER_DEBUG_TRACE=1 ./ze_init_test
Loading Driver libze_intel_gpu.so.1 succeeded from: /lib/x86_64-linux-gnu/libze_intel_gpu.so.1
Tracing Layer Library Path: libze_tracing_layer.so.1
ze_lib Context Init() zeInitDrivers or zeInit failed with ZE_RESULT_ERROR_UNINITIALIZED
zeInit: 0x78000001     ← ZE_RESULT_ERROR_UNINITIALIZED
```

- Level-zero loader successfully dlopens `libze_intel_gpu.so.1`
  (the NEO GPU adapter). The adapter then calls into the kernel
  i915 / xe driver via the standard `/dev/dri/renderD<N>` syscalls
  and `EINVAL` is returned for the device-init ioctl. NEO 25.18
  surfaces this as `ZE_RESULT_ERROR_UNINITIALIZED`.
- `/dev/dri` is correctly bind-mounted (ADR-0528). renderD129 (Arc
  PCI 03:00.0) is mode 0666; the container runs as root, so it's not
  a permission issue.
- Confirmed against `clinfo`: `libigdrcl.so` (Intel OpenCL GPU ICD)
  is loaded by the Khronos ICD loader but its `clIcdGetPlatformIDs`
  fails — same root cause as the level-zero failure (NEO ↔ kernel
  UAPI mismatch).

### 2. SYCL CPU OpenCL: libtbb.so.12 missing from LD_LIBRARY_PATH

The Intel CPU OpenCL driver at
`/opt/intel/oneapi/compiler/latest/lib/libintelocl.so` dlopens
`libtbb.so.12` at OpenCL platform-enumeration time:

```text
OCL_ICD_ENABLE_TRACE=1 clinfo …
Failed to load driver because libtbb.so.12: cannot open shared object file
```

`libtbb.so.12` is present in `/opt/intel/oneapi/tbb/latest/lib/`
(part of the `intel-basekit` install) but the previous
`LD_LIBRARY_PATH` only carried `compiler/latest/lib`,
`umf/latest/lib`, and `tcm/latest/lib`. Adding `tbb/latest/lib`
restores the Intel CPU OpenCL platform.

### 3. HIP: ROCm userspace too old for kernel-7.x KFD ioctl ABI

The container shipped ROCm 6.4 from
`https://repo.radeon.com/rocm/apt/6.4`. Host runs ROCm 7.2.3.

```text
$ rocminfo
ROCk module is loaded
Unable to open /dev/kfd read-write: Invalid argument
Failed to get user name to check for video group membership
```

- `/dev/kfd` is mode 0600 root:render but the container runs as
  root with group-add render — passthrough is not the failure.
- `Invalid argument` (`EINVAL`) from `open(/dev/kfd, O_RDWR)` on
  Linux ≥ 7.0 is the canonical signal of an ioctl ABI mismatch:
  the kernel KFD module's UAPI revs across ROCm major versions,
  and 6.x userspace cannot negotiate the 7.x version handshake.
- Verified that `repo.radeon.com/rocm/apt/7.2.3 noble main` is
  published and carries `rocm-hip-runtime-dev` + `hipcc` +
  `rocm-cmake` (the same three packages we already install).

## Decision matrix

See [ADR-0541 §Alternatives considered](../adr/0541-dev-container-sycl-hip-runtime-fix.md#alternatives-considered)
for the full comparison of Intel APT vs GitHub-release vs distro
packages and the ROCm version-pin alternatives.

Summary: only GitHub-release .debs of NEO 26.18 (+ matching
IGC 2.34.4 + gmmlib 22.10.0) supply a version that works on Linux
≥ 7.0 as of 2026-05-18; only ROCm 7.x speaks the kernel-7.x KFD
ioctls. The Intel APT noble/unified repo is too far behind for the
current kernel.

## Reproducer

```bash
# Build container with the fix
docker compose --project-directory $(git rev-parse --show-toplevel) \
    -f dev/docker-compose.yml build dev-mcp
docker compose --project-directory $(git rev-parse --show-toplevel) \
    -f dev/docker-compose.yml up -d

# Verify backends
docker exec vmaf-dev-mcp bash -c '
  for B in sycl hip; do
    vmaf --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
         --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
         --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
         --backend $B --json --output /tmp/be_$B.json
    echo "$B: rc=$?"
  done'
# Expect: real GPU dispatch, no "using CPU" / "fallback" messages,
# scores within places=5 (sycl) / places=4 (hip) of CPU per ADR-0214.
```

The entrypoint banner now also emits `SYCL level_zero:gpu detected`
and `HIP HSA agent detected` (or matching WARN lines) on every
container start.

## Forward maintenance

- When kernel N+1 ships and breaks NEO 26.18 / ROCm 7.2.3, bump
  `ARG NEO_VER` / `ARG IGC_VER` / `ARG GMMLIB_VER` / `ARG ROCM_VER`
  in `dev/Containerfile`. The entrypoint visibility probe surfaces
  the regression on container start, so the bump is easy to time.
- Eventually, Intel's `noble/unified` APT repo should catch up to
  the GitHub-release tag. At that point we can simplify the
  Containerfile back to a one-line APT install for Intel. Track in
  `dev/AGENTS.md` "Userspace ↔ host-kernel UAPI version pins".
