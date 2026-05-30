# AGENTS.md — dev/ (container infra)

Invariants for the `dev/` tree that agents must preserve across rebases and
follow-up PRs. See [Research-0135](../docs/research/0135-dev-mcp-container-stage-3-fix-2026-05-16.md)
for the diagnosis that established these.

## Containerfile invariants

### USER ordering and directory ownership (stage 3)

`WORKDIR` always creates directories as **root**, regardless of any previous
`USER` directive. After `COPY --chown=vmaf:vmaf . /dest/`, only the
*contents* are owned by `vmaf`; the destination directory itself is still
owned by root.

**Rule**: Any `RUN` step that executes as a non-root user and needs to
create a subdirectory inside a `WORKDIR`-created path must be preceded by:

```dockerfile
RUN chown <user>:<group> /parent /parent/dest
USER <user>
```

Do NOT rely on `COPY --chown` alone to make a directory writable — it does
not change the directory entry's owner, only file/subdirectory contents.

Violating this causes `meson setup build` (and any other tool that calls
`os.makedirs`) to fail with `PermissionError: [Errno 13] Permission denied`
at exactly the build-dir creation step, exit code 13.

### CUDA package names

- Use `cuda-toolkit` (the current unversioned meta-package).
- Do NOT install `libcuda1` (the runtime driver) — it must come from
  `nvidia-container-runtime` at run-time; baking it in shadows the host driver.
- Do NOT install `cuda-compiler` — this is a legacy alias that no longer
  exists in the NVIDIA CUDA channels; `cuda-toolkit` already provides `nvcc`.

### Intel oneAPI package name

- Use `intel-basekit` (unversioned meta-package).
- Do NOT use `intel-basekit-<year>.<quarter>` (e.g., `intel-basekit-2025.3`):
  Intel does not publish year-quarter-versioned meta-package names in
  `apt.repos.intel.com/oneapi`. Using the versioned name causes
  `E: Unable to locate package`.

### ROCm / HIP package names

- Use `rocm-hip-runtime-dev` (not `rocm-hip-sdk`).
- `rocm-hip-sdk` transitively installs `rccl` (multi-GPU collectives) which
  depends on `libdrm-amdgpu-amdgpu1` + `libdrm2-amdgpu` — packages absent
  from the ROCm noble apt repo. libvmaf HIP kernels use one GPU per
  worker; rccl is not needed.

### Userspace ↔ host-kernel UAPI version pins (ADR-0541)

The Intel NEO compute-runtime and ROCm KFD userspace MUST match the host
kernel's i915 / xe / KFD ioctl ABI, or `vmaf --backend sycl|hip` silently
falls back to CPU. Two hard pins live in `dev/Containerfile`:

- **`ARG NEO_VER=26.18.38308.1`** (+ `IGC_VER=2.34.4+21428` +
  `GMMLIB_VER=22.10.0`). Pinned via GitHub releases because Intel's
  `noble/unified` APT repo's newest as of 2026-05-18 is `25.18.x`, too old
  for kernel ≥ 7.0. The IGC / gmmlib versions follow the NEO release-notes
  manifest — bump them together when you bump NEO.
- **`ARG ROCM_VER=7.2.3`** for `https://repo.radeon.com/rocm/apt/${ROCM_VER}`.
  ROCm 6.x KFD ioctls do not match kernel ≥ 7.0.

When CI / a maintainer's host runs a newer kernel that breaks these pins,
the `dev-mcp-entrypoint.sh` runtime-visibility probe (also ADR-0541)
surfaces the regression on container start as
`WARN: SYCL level_zero:gpu NOT detected` or `WARN: HIP HSA agent NOT
detected`. Bump the relevant ARG and rebuild.

### SHELL / hadolint DL4006

- `SHELL ["/bin/bash", "-o", "pipefail", "-c"]` is set in the `gpu-sdks`
  stage and **inherited** by `libvmaf-build` and `dev-mcp`.
- hadolint does not track cross-stage SHELL inheritance. Any `RUN` step in
  a derived stage that contains a pipe will trigger DL4006 as a false positive.
  Suppress with `# hadolint ignore=DL4006` and note that SHELL is inherited.

### GPU backend exposure invariants (ADR-0514 / Research-0138)

These four constraints must survive every rebase. Each one corresponds to
a real container-side regression that hid a host GPU from libvmaf:

1. **`LD_LIBRARY_PATH` must include `${ONEAPI_ROOT}/tcm/latest/lib`.** The
   oneAPI level-zero UR adapter dlopens `libhwloc.so.15` at adapter-load
   time and that library lives only in `tcm/latest/lib` (not the
   `compiler/latest/lib` or `umf/latest/lib` paths the older env block
   covered). Dropping it silently breaks SYCL across every Intel GPU even
   if the device passthrough is otherwise correct.

2. **Do NOT set `VK_ICD_FILENAMES` or `VK_DRIVER_FILES` in the image.**
   The Vulkan loader's default search of `/etc/vulkan/icd.d/` +
   `/usr/share/vulkan/icd.d/` picks up both the NVIDIA Container
   Toolkit's run-time bind-mount AND the mesa intel/radeon/lavapipe
   ICDs. Pinning either env var to a single file (especially the prior
   `lvp_icd.x86_64.json`, which does not exist on disk) hides every
   real GPU. Operators that need to force a single ICD can set the
   env var at `docker exec` time per-invocation.

3. **`/dev/dri` is bind-mounted as a whole directory in
   `dev/docker-compose.yml` (ADR-0528).** Docker's `devices:` directive
   carries leaf device nodes but drops subdirectory entries such as
   `by-path/` and `by-id/`. The Intel compute-runtime discovers Arc GPUs
   through the udev-managed `pci-XXXX:YY:ZZ.W-render` symlinks inside
   `by-path/`; without them sycl-ls reports `Platforms: 0` even when
   `/dev/dri/renderD*` is visible. The former `/dev/dri/by-path`-only
   bind (ADR-0514) was vulnerable to PCI re-enumeration after reboot,
   suspend/resume, or GPU hotplug — the path would no longer exist and
   the container would fail to start. The fix mounts the stable
   `/dev/dri` directory itself (a kernel devtmpfs entry that is always
   present) and drops the separate `devices: /dev/dri:/dev/dri` entry
   (the bind-mount subsumes it). Only `/dev/kfd` remains under
   `devices:` (single leaf node, no subdirectory dependency).

4. **The build-time backend probe loop in stage 3 must stay green for
   `cpu` + `cuda` and `WARN`-but-not-`built without X support` for the
   GPU backends.** The probe runs vmaf against the Netflix golden CPU
   pair with `--backend cpu cuda sycl vulkan hip` and `|| echo WARN`s
   on missing devices. The signal we care about is the precise
   `built without X support` string — that means a meson flag silently
   flipped off and a real backend disappeared from libvmaf entirely
   (the precise failure mode that triggered ADR-0514 for HIP).

### FFmpeg encoder exposure invariants (ADR-0541)
### Full GPU backend plumbing invariants (ADR-0541)

Four constraints that close the last silent-fallback gaps surfaced
empirically against the dev machine
(NVIDIA RTX 4090, Intel Arc A380, AMD `gfx1036`). Each one
corresponds to a backend that would otherwise land on CPU / lavapipe
/ `-ENODEV` despite the device being visible to the kernel:

1. **The entrypoint's `VK_DRIVER_FILES` rewrite must stay in place.**
   `dev/scripts/dev-mcp-entrypoint.sh` enumerates every JSON under
   `/etc/vulkan/icd.d/` + `/usr/share/vulkan/icd.d/`, drops anything
   matching `lvp_*` / `lavapipe*`, and pins `VK_DRIVER_FILES` to the
   colon-separated allowlist of real ICDs. Without the rewrite,
   The Vulkan backend was dropped per ADR-0726.
   software ICD on multi-vendor hosts where lavapipe sorts before the
   real GPU ICDs. Do NOT replace this with a static `ENV
   VK_DRIVER_FILES=…` in the Containerfile — operators on CPU-only
   hosts (no real ICD visible) need lavapipe to remain the fallback;
   the entrypoint's "if any real ICD exists" guard preserves that.
2. **`HSA_OVERRIDE_GFX_VERSION=10.3.0` must stay in
   `dev/docker-compose.yml` `common-env`.** AMD `gfx1036` (Raphael
   iGPU, RDNA2 IP rev 10.3.6) is not on the ROCm 6.x supported-GPU
   allowlist. Without the override, `hsa_init()` returns
   `HSA_STATUS_ERROR_OUT_OF_RESOURCES` and `rocminfo` reports "Unable
   to open /dev/kfd read-write: Invalid argument" even though
   `/dev/kfd` is bind-mounted via the `devices:` block and the
   video / render groups are joined. `HSA_ENABLE_SDMA=0` (RDNA2 iGPU
   SDMA-fault mitigation) and `ROCR_VISIBLE_DEVICES=0` (pin HIP to
   the single AMD adapter) accompany the override and should not be
   trimmed.
3. **`intel-media-va-driver-non-free` + `mesa-va-drivers` must stay
   in the stage-1 apt list.** The Intel compute-runtime
   (`libze_intel_gpu.so.1`) dlopens
   `/usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so` during
   `zeInit()`-time GPU capability probing. Without
   `intel-media-va-driver-non-free`, `vaInitialize()` returns
   `VA_STATUS_ERROR_UNKNOWN`, the compute-runtime bails out of L0
   driver enumeration, and `sycl-ls` reports `Platforms: 0` on Intel
   Arc hosts. `mesa-va-drivers` provides `radeonsi_drv_video.so` for
   the AMD equivalent.
4. **`NVIDIA_DRIVER_CAPABILITIES` must include `graphics` (in
   addition to `compute,utility,video`).** The NVIDIA Container
   Toolkit only bind-mounts `nvidia_icd.json` into
   `/etc/vulkan/icd.d/` when the `graphics` token is set. Trimming
   the env block to `compute,utility` silently disables NVIDIA Vulkan
   while leaving CUDA + nvidia-smi working — a particularly hard
   regression to spot because every other lane stays green. The
   compose-file `common-env` block carries the full token set with
   an inline comment; do NOT trim it.

### FFmpeg encoder exposure invariants (ADR-0540)

These constraints must survive every rebase. Each one corresponds to a
real encoder or FFmpeg integration path that the `vmaf-tune compare`
predicate would silently skip, or that the dev-container FFmpeg build
would fail to compile:

1. **SVT-AV1 must be built from source; the apt `libsvtav1-dev`
   package is NOT sufficient.** Ubuntu's `libsvtav1-dev`
   (1.7.0+dfsg-2build1) omits `SvtAv1Enc.pc` (verified 2026-05-18
   against `ubuntu:24.04`). FFmpeg's `require_pkg_config libsvtav1
   SvtAv1Enc ...` probe therefore fails. SVT-AV1 is cloned from
   `https://gitlab.com/AOMediaCodec/SVT-AV1.git` at pinned tag
   (`v2.1.0` at time of writing) and built with cmake under
   `/usr/local/`. `cmake --install` writes `SvtAv1Enc.pc` to
   `/usr/local/lib/pkgconfig/` as a side effect. Do NOT replace
   with the distro package.

   **libaom is intentionally NOT enabled.** The fork's
   `ffmpeg-patches/0007` references libaom `aom_roi_map_t` fields
   that do not exist in any released libaom version. SVT-AV1
   covers the production AV1 lane. Re-enabling libaom requires
   first fixing patch 0007's ROI helper to either target a real
   libaom version or gate the ROI bridge behind a version probe.
2. **`libvvenc` (Fraunhofer VVC reference) must be built from source
   and installed under `/usr/local`.** The package is not in Ubuntu
   apt. Pin to a release tag (`v1.14.0` as of ADR-0568; bumped from
   `v1.12.0` 2026-05-18) so future rebases get a deterministic build.
   The configure-time check is `check_pkg_config(libvvenc, ...)`
   which needs the `.pc` file `VVENC_ENABLE_INSTALL=ON` ships.
3. **AMF headers vendored from the upstream `GPUOpen-Libraries-
   AndSDKs/AMF` repo (header-only).** FFmpeg's `--enable-amf` needs
   only the headers at compile time; `libamfrt64.so` runtime
   resolution is host-side. Do NOT try to install `libamfrt64.so` from
   apt — it lives in the proprietary `amdgpu-pro` userspace and is
   not packaged.
4. **QSV needs both the oneVPL dispatcher and the GPU runtime.**
   `libvpl-dev` provides `libvpl.so.2` and lets FFmpeg compile
   `--enable-libvpl`, but it does not provide the Gen implementation
   (`libmfx-gen.so`) that creates an Arc/iGPU MFX session at runtime.
   `dev/Containerfile` builds `intel/vpl-gpu-rt` at the pinned
   `VPL_GPU_RT_TAG` and installs it into
   `/usr/lib/x86_64-linux-gnu/`, the path searched by Ubuntu's
   `libvpl.so.2` dispatcher. Do NOT move it back to `/usr/local/lib`
   without also preserving dispatcher discovery, or QSV will regress
   to `Error creating a MFX session: -9` while still appearing in
   `ffmpeg -encoders`.
5. **The FFmpeg configure line carries all of `--enable-nvenc
   --enable-cuda-nvcc --enable-libvpl --enable-amf` in addition to
   the software codec flags.** Dropping any one silently disappears
   a hardware-encoder family from the `ffmpeg -encoders` listing and
   breaks the `vmaf-tune compare` sweep. The build-time encoder
   probe at the end of stage 3.5 catches drops with `WARN <encoder>
   missing` lines. Do NOT add `--enable-libnpp` — FFmpeg n8.1.1's
   NPP support tops out at CUDA 12.x, and the image's
   `cuda-toolkit` meta-package tracks 13.x, so passing the flag
   hard-errors at configure time. `scale_cuda` (built via
   `--enable-cuda-nvcc`) is the replacement for the
   `scale_npp` pipeline.
6. **The FFmpeg SYCL patch must use the current libvmaf state-free
   ownership contract.** `libvmaf_sycl.h` declares
   `vmaf_sycl_state_free(VmafSyclState **sycl_state)`, matching
   Vulkan / HIP / Metal rather than CUDA. Keep
   `ffmpeg-patches/0003-*` calling
   `vmaf_sycl_state_free(&s->sycl_state)`. Passing the single pointer
   builds against stale patch text and fails the container FFmpeg
   compile with `-Wincompatible-pointer-types`.

### Compose healthcheck invariant (ADR-0641)

The `dev-mcp` service healthcheck must match the entrypoint transport.
The entrypoint exposes MCP over stdio (`docker exec -i vmaf-dev-mcp
/opt/vmaf-venv/bin/vmaf-mcp`) and does not create
`/sockets/vmaf-mcp.sock` by default. Therefore the compose healthcheck
must remain a CLI check (`vmaf --version`). Reverting it to
`test -S /sockets/vmaf-mcp.sock` leaves the container permanently
`unhealthy` and prevents `smoke-probe-cron` from starting even though
the runtime is usable.

### Runtime dependency invariants (ADR-0541 / ADR-0568)

1. **`LD_LIBRARY_PATH` must include `${ONEAPI_ROOT}/tbb/latest/lib`
   (ADR-0541).** The Intel CPU OpenCL ICD
   (`/opt/intel/oneapi/compiler/latest/lib/libintelocl.so`) dlopens
   `libtbb.so.12` at OpenCL platform-enumeration time. Without
   `tbb/latest/lib` the Khronos ocl-icd loader silently drops the Intel
   CPU OpenCL platform, leaving SYCL with no CPU fallback when the GPU
   path is also degraded. The full env line in the Containerfile is now
   `${DPCPP_ROOT}/lib:${ONEAPI_ROOT}/umf/latest/lib:${ONEAPI_ROOT}/tcm/latest/lib:${ONEAPI_ROOT}/tbb/latest/lib:${LD_LIBRARY_PATH}`.

2. **NEO + ROCm userspace are version-pinned to the host kernel's UAPI
   (ADR-0541).** See "Userspace ↔ host-kernel UAPI version pins" above.
   The `dev-mcp-entrypoint.sh` banner emits a `WARN: SYCL level_zero:gpu
   NOT detected` / `WARN: HIP HSA agent NOT detected` line at container
   start when the pin no longer matches the host — bump the ARG and
   rebuild instead of working around it on the host (CLAUDE.md §12 r15
   sub-rule 4).

3. **`ORT_VERSION` must satisfy the `ai/pyproject.toml` requirement
   `onnxruntime>=1.20,<2.0`.** Current pin: `1.26.0` (bumped from 1.20.1
   per ADR-0568 2026-05-18). The tarball naming pattern is
   `onnxruntime-linux-x64-${ORT_VERSION}.tgz` from the microsoft/onnxruntime
   GitHub releases. The C API is stable across the 1.x line; however,
   the ROCm EP and CUDA EP are only available from ORT 1.26+ (matching the
   container's ROCm 7.2.3 + CUDA 13.x stack set in ADR-0541/ADR-0542).
   When bumping ORT_VERSION: verify the new version's tarball exists at the
   GitHub releases URL before updating the ARG, and update this note.
