- **`vmaf-dev-mcp` container: SYCL + HIP no longer silently fall
  back to CPU on Linux kernel ≥ 7.0 hosts.** Five-part fix in
  `dev/Containerfile` + `dev/docker-compose.yml` (ADR-0541):
  (1) Intel NEO compute-runtime pinned to `26.18.38308.1` (with
  matching IGC `2.34.4` + gmmlib `22.10.0`) via GitHub releases —
  Intel's `noble/unified` APT repo carries only up to 25.18 as of
  2026-05-18, too old for kernel-7.x i915 / xe UAPI (`zeInit()`
  returned `ZE_RESULT_ERROR_UNINITIALIZED`, `sycl-ls Platforms: 0`).
  (2) ROCm bumped from 6.4 → 7.2.3 to match the host KFD ioctl
  ABI (rocminfo previously failed with `Unable to open /dev/kfd
  read-write: Invalid argument`). (3) `/opt/intel/oneapi/tbb/latest/lib`
  added to `LD_LIBRARY_PATH` so the Intel CPU OpenCL ICD loads
  (`libintelocl.so` dlopens `libtbb.so.12` at platform enumeration).
  (4) `dev/docker-compose.yml` adds `security_opt: seccomp=unconfined`
  and re-adds `devices: /dev/dri:/dev/dri` (NEO 26.x + ROCm 7.x both
  use syscalls Docker's default seccomp profile blocks, and the
  whole-directory bind-mount alone doesn't add the cgroup device
  whitelist NEO 26.x requires to actually init the GPU even when the
  device file is mode 0666). (5) `dev/scripts/dev-mcp-up.sh` now
  reads the host's actual video / render GIDs (Arch / CachyOS /
  Fedora differ from the Ubuntu 44 / 109 defaults). The
  `dev-mcp-entrypoint.sh` script gains an ADR-0541 visibility probe
  that retries up to 5× over ~10 s and emits a `WARN: SYCL
  level_zero:gpu NOT detected` / `WARN: HIP HSA agent NOT detected`
  banner when the GPU is missing on container start, so future host-
  kernel / userspace ABI mismatches surface immediately instead of
  as CPU-scores-under-a-GPU-tagged-JSON. Verified end-to-end on the
  dev host (Intel Arc A380 + AMD gfx1036, CachyOS Linux 7.0.8):
  `vmaf --backend sycl` matches CPU within places=5
  (ADR-0214 gate); `vmaf --backend hip` reaches HSA agent init but
  fails at HSACO load because the built-in fat binaries don't carry
  `gfx1036` (separate HSACO-build follow-up, not this PR's scope —
  the kernel ABI is now operational).
