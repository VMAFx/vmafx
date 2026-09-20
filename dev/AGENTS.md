# AGENTS.md — dev/ (container infra)

Parent: [../AGENTS.md](../AGENTS.md).

Invariants for `dev/` tree agents must preserve across rebases and
follow-up PRs. See [Research-0135](../docs/research/0135-dev-mcp-container-stage-3-fix-2026-05-16.md)
for diagnosis establishing these.

## Containerfile invariants

### USER ordering and directory ownership (stage 3)

`WORKDIR` always creates directories as **root**, regardless of any
previous `USER` directive. After `COPY --chown=vmaf:vmaf . /dest/`,
only *contents* owned by `vmaf`; destination directory itself still
owned by root.

**Rule**: any `RUN` step executing as non-root user and needing to
create subdirectory inside `WORKDIR`-created path must be preceded
by:

```dockerfile
RUN chown <user>:<group> /parent /parent/dest
USER <user>
```

Do NOT rely on `COPY --chown` alone to make directory writable —
does not change directory entry's owner, only file/subdirectory
contents.

Violating this causes `meson setup build` (and any other tool
calling `os.makedirs`) to fail with
`PermissionError: [Errno 13] Permission denied` at exactly build-dir
creation step, exit code 13.

### CUDA package names

- Use `cuda-toolkit` (current unversioned meta-package).
- Do NOT install `libcuda1` (runtime driver) — must come from
  `nvidia-container-runtime` at run-time; baking it in shadows host
  driver.
- Do NOT install `cuda-compiler` — legacy alias no longer existing in
  NVIDIA CUDA channels; `cuda-toolkit` already provides `nvcc`.

### Intel oneAPI package name

- Use `intel-basekit` (unversioned meta-package).
- Do NOT use `intel-basekit-<year>.<quarter>` (e.g.,
  `intel-basekit-2025.3`): Intel doesn't publish
  year-quarter-versioned meta-package names in
  `apt.repos.intel.com/oneapi`. Versioned name causes
  `E: Unable to locate package`.

### ROCm / HIP package names

- Use `rocm-hip-runtime-dev` (not `rocm-hip-sdk`).
- `rocm-hip-sdk` transitively installs `rccl` (multi-GPU
  collectives), depends on `libdrm-amdgpu-amdgpu1` +
  `libdrm2-amdgpu` — packages absent from ROCm noble apt repo.
  libvmaf HIP kernels use one GPU per worker; rccl not needed.

### Userspace ↔ host-kernel UAPI version pins (ADR-0541)

Intel NEO compute-runtime and ROCm KFD userspace MUST match host
kernel's i915 / xe / KFD ioctl ABI, or `vmaf --backend sycl|hip`
silently falls back to CPU. Two hard pins live in
`dev/Containerfile`:

- **`ARG NEO_VER`** and shared `LEVEL_ZERO_VERSION`.
  Pinned via GitHub releases: Intel's `noble/unified` APT repo's
  newest as of 2026-05-18 = `25.18.x`, too old for kernel ≥ 7.0.
  Level Zero loader comes from `oneapi-src/level-zero`. Its
  SDK-stage `RUN` sources copied root `build-config.env` from
  `/opt/vmafx/`; both download URL components use
  `LEVEL_ZERO_VERSION`. Do not reintroduce separate
  `LEVEL_ZERO_VER`/`LEVEL_ZERO_VERSION` ARG or literal version.
  `scripts/ci/check-workflow-versions.py` and single-source fixture
  gate protect this consumer. Renovate owns setting only in
  `build-config.env`. **Invariant (ADR-1145)**: `NEO_VER` = only
  pinned Intel version; never reintroduce `GMMLIB_VER` or `IGC_VER`
  ARGs. Matching `gmmlib` and `IGC` deb packages dynamically
  derived, verified against published sha256 checksums at build time
  by `dev/scripts/fetch-intel-neo.py`.
  **GitHub credential transport (ADR-1271):** the optional API token is the
  BuildKit secret `github_token`, exposed as `GITHUB_TOKEN` only to the NEO
  fetch `RUN`. Never reintroduce `ARG GITHUB_TOKEN`, `ENV GITHUB_TOKEN`, or a
  token-valued `--build-arg`. Raw anonymous builds omit `--secret`; Compose
  maps the host variable and treats unset or empty input as anonymous. Keep
  `scripts/ci/check-dev-container-build-secret.py`, its fixture tests, and the
  Docker/Compose `--check` workflow steps wired together.
- **Digest-pinned `rocm-src` stage**
  (`rocm/dev-ubuntu-26.04:10.0.0-full`) replaces old `ARG ROCM_VER` +
  `repo.radeon.com/rocm/apt/` install. **Invariant (ADR-1225 /
  ADR-1231)**: keep selected digest-pinned image as SDK source.
  Update `ROCM_BUILDER` / `ROCM_RUNTIME` in `build-config.env`,
  regenerate their mirrors. ADR-1225 records historical
  package-channel checks; those observations aren't current
  inventory of AMD's release channels. **Invariant**: keep prune
  list in `rocm-src` stage, but never prune
  `librocprofiler-register` — `libamdhip64.so` links it, dropping it
  makes every HIP binary fail at load with "cannot open shared
  object file". Keep post-prune HIP compile/link and host-entry
  smoke in this stage; `hipconfig --version` alone doesn't exercise
  compiler dependency closure. Smoke compiles kernel but doesn't
  launch it or require GPU. ROCm 6.x KFD ioctls don't match kernel ≥
  7.0; 10.0.0 does (verified on Linux 7.2.3 with `gfx1036`).

CI / maintainer's host running newer kernel that breaks these pins
-> `dev-mcp-entrypoint.sh` runtime-visibility probe (also ADR-0541)
surfaces regression on container start as
`WARN: SYCL level_zero:gpu NOT detected` or `WARN: HIP HSA agent NOT
detected`. Bump relevant version owner, rebuild.

**The probe runs argv, never a shell string.** `_probe_with_retry` in
`scripts/dev-mcp-entrypoint.sh` takes one program name and runs it as
`"${prog}"`. The earlier `eval "${cmd}"` form was removed by PR #350, came
back through a stale squash-merge (PR #414) and was removed again on
2026-09-19: the entrypoint is PID 1 with the container's whole environment,
so a probe value that ever comes from configuration would be command
injection. A probe that needs flags gets explicit argv handling in the
function; do not reintroduce `eval` or `bash -c`. Keep the function at top
level with the opening line `_probe_with_retry() {` —
`scripts/ci/tests/test-dev-mcp-entrypoint-probe.sh` (pre-commit hook
`test-dev-mcp-entrypoint-probe`) extracts it by that line.

### SHELL / hadolint DL4006

- Declare `SHELL ["/bin/bash", "-o", "pipefail", "-c"]` explicitly in
  `build-deps`, `gpu-sdks`, `libvmaf-build`, `go-build`, and
  `dev-mcp`. Each executing stage then exposes pipeline failure
  contract to both builder and static analysis without depending on
  parent-stage tracking.
- Keep Go output-count guard as Bash array, not parsed `ls` output.
  Artifact builder returns to `USER vmaf` after privileged
  compilation; final runtime also remains `USER vmaf`.
- Keep collection failure handling explicit: import failure stops
  layer; pytest collection failure prints captured diagnostics, exits
  nonzero.

### Source-tree paths after ADR-0700 / ADR-0870

- Library source tree renamed from `libvmaf/` to `core/` per
  ADR-0700. Python harness split into `compat/python-vmaf/` package +
  `python/` shim. Containerfile reflects this:
  - `COPY core/        /build/vmaf/core/` (was `libvmaf/`).
  - `COPY compat/      /build/vmaf/compat/` (required for editable
    Python install through `python/` shim).
  - `meson setup core/build core` / `ninja -C core/build install`.
- **Rule**: any rebase picking up upstream patch touching old
  `libvmaf/` directory must rewrite path to `core/` before applying
  it to Containerfile's COPY/source/build paths. `.dockerignore`
  carries both `core/build*/` and legacy `libvmaf/build*/` siblings
  so pre-rename worktree still excludes its build dirs; do not
  delete legacy entries.
- See [ADR-0870](../docs/adr/0870-helm-values-schema-and-container-rebuild-audit.md)
  for audit establishing this invariant after drift went undetected
  through several merge trains.

### GPU backend exposure invariants (ADR-0514 / Research-0138)

These four constraints must survive every rebase. Each corresponds
to real container-side regression that hid host GPU from libvmaf:

1. **`LD_LIBRARY_PATH` must include `${ONEAPI_ROOT}/tcm/latest/lib`.**
   oneAPI level-zero UR adapter dlopens `libhwloc.so.15` at
   adapter-load time; library lives only in `tcm/latest/lib` (not
   `compiler/latest/lib` or `umf/latest/lib` paths older env block
   covered). Dropping it silently breaks SYCL across every Intel GPU
   even if device passthrough otherwise correct.

2. **Do NOT set `VK_ICD_FILENAMES` or `VK_DRIVER_FILES` in image.**
   Vulkan loader's default search of `/etc/vulkan/icd.d/` +
   `/usr/share/vulkan/icd.d/` picks up both NVIDIA Container
   Toolkit's run-time bind-mount AND mesa intel/radeon/lavapipe
   ICDs. Pinning either env var to single file (especially prior
   `lvp_icd.x86_64.json`, which doesn't exist on disk) hides every
   real GPU. Operators needing to force single ICD can set env var
   at `docker exec` time per-invocation.

3. **`/dev/dri` bind-mounted as whole directory in
   `dev/docker-compose.yml` (ADR-0528).** Docker's `devices:`
   directive carries leaf device nodes but drops subdirectory
   entries such as `by-path/` and `by-id/`. Intel compute-runtime
   discovers Arc GPUs through udev-managed
   `pci-XXXX:YY:ZZ.W-render` symlinks inside `by-path/`; without
   them sycl-ls reports `Platforms: 0` even when
   `/dev/dri/renderD*` visible. Former `/dev/dri/by-path`-only bind
   (ADR-0514) vulnerable to PCI re-enumeration after reboot,
   suspend/resume, or GPU hotplug — path would no longer exist,
   container would fail to start. Fix mounts stable `/dev/dri`
   directory itself (kernel devtmpfs entry always present), drops
   separate `devices: /dev/dri:/dev/dri` entry (bind-mount subsumes
   it). Only `/dev/kfd` remains under `devices:` (single leaf node,
   no subdirectory dependency).

4. **Build-time backend probe loop in stage 3 must stay green for
   `cpu` + `cuda`, `WARN`-but-not-`built without X support` for GPU
   backends.** Probe runs vmaf against Netflix golden CPU pair with
   `--backend cpu cuda sycl hip` and `|| echo WARN`s on missing
   devices. Signal we care about = precise `built without X
   support` string. Means meson flag silently flipped off, real
   backend disappeared from libvmaf entirely (precise failure mode
   that triggered ADR-0514 for HIP).

### FFmpeg encoder exposure invariants (ADR-0541)

### Full GPU backend plumbing invariants (ADR-0541)

Four constraints closing last silent-fallback gaps surfaced
empirically against dev machine (NVIDIA RTX 4090, Intel Arc A380,
AMD `gfx1036`). Each corresponds to backend that would otherwise
land on CPU / lavapipe / `-ENODEV` despite device being visible to
kernel:

1. **Entrypoint's `VK_DRIVER_FILES` rewrite must stay in place.**
   `dev/scripts/dev-mcp-entrypoint.sh` enumerates every JSON under
   `/etc/vulkan/icd.d/` + `/usr/share/vulkan/icd.d/`, drops anything
   matching `lvp_*` / `lavapipe*`, pins `VK_DRIVER_FILES` to
   colon-separated allowlist of real ICDs. Without rewrite: software
   ICD wins on multi-vendor hosts where lavapipe sorts before real
   GPU ICDs. (Vulkan backend dropped per ADR-0726.) Do NOT replace
   this with static `ENV VK_DRIVER_FILES=…` in Containerfile —
   operators on CPU-only hosts (no real ICD visible) need lavapipe
   to remain fallback; entrypoint's "if any real ICD exists" guard
   preserves that.
2. **`HSA_OVERRIDE_GFX_VERSION` must NOT be reintroduced in
   `dev/docker-compose.yml` `common-env` (ADR-1225).** Under ROCm
   6.x override was mandatory: AMD `gfx1036` (Raphael iGPU, RDNA2 IP
   rev 10.3.6) wasn't on supported-GPU allowlist, so `hsa_init()`
   returned `HSA_STATUS_ERROR_OUT_OF_RESOURCES`, `rocminfo` reported
   "Unable to open /dev/kfd read-write: Invalid argument" even with
   `/dev/kfd` bind-mounted and video / render groups joined. ROCm 10
   supports `gfx1036` natively; override now actively harmful. Would
   alias agent to `gfx1030` while meson compiles `gfx1036` code
   objects for arch `rocm_agent_enumerator` reports.
   `HSA_ENABLE_SDMA=0` (RDNA2 iGPU SDMA-fault mitigation) and
   `ROCR_VISIBLE_DEVICES=0` (pin HIP to single AMD adapter) stay,
   should not be trimmed.
3. **`intel-media-va-driver-non-free` + `mesa-va-drivers` must stay
   in stage-1 apt list.** Intel compute-runtime
   (`libze_intel_gpu.so.1`) dlopens
   `/usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so` during
   `zeInit()`-time GPU capability probing. Without
   `intel-media-va-driver-non-free`: `vaInitialize()` returns
   `VA_STATUS_ERROR_UNKNOWN`, compute-runtime bails out of L0 driver
   enumeration, `sycl-ls` reports `Platforms: 0` on Intel Arc hosts.
   `mesa-va-drivers` provides `radeonsi_drv_video.so` for AMD
   equivalent.
4. **`NVIDIA_DRIVER_CAPABILITIES` must include `graphics` (in
   addition to `compute,utility,video`).** NVIDIA Container Toolkit
   only bind-mounts `nvidia_icd.json` into `/etc/vulkan/icd.d/` when
   `graphics` token set. Trimming env block to `compute,utility`
   silently disables NVIDIA Vulkan while leaving CUDA + nvidia-smi
   working — particularly hard regression to spot: every other lane
   stays green. Compose-file `common-env` block carries full token
   set with inline comment; do NOT trim it.

### FFmpeg encoder exposure invariants (ADR-0540)

These constraints must survive every rebase. Each corresponds to
real encoder or FFmpeg integration path that `vmaf-tune compare`
predicate would silently skip, or that dev-container FFmpeg build
would fail to compile:

1. **SVT-AV1 must be built from source; apt `libsvtav1-dev` package
   NOT sufficient.** Ubuntu's `libsvtav1-dev` (1.7.0+dfsg-2build1)
   omits `SvtAv1Enc.pc` (verified 2026-05-18 against `ubuntu:24.04`).
   FFmpeg's `require_pkg_config libsvtav1
   SvtAv1Enc ...` probe therefore fails. SVT-AV1 cloned from
   `https://gitlab.com/AOMediaCodec/SVT-AV1.git` at pinned tag
   (`v2.1.0` at time of writing), built with cmake under
   `/usr/local/`. `cmake --install` writes `SvtAv1Enc.pc` to
   `/usr/local/lib/pkgconfig/` as side effect. Do NOT replace with
   distro package.

   **libaom intentionally NOT enabled.** Fork's
   `ffmpeg-patches/0007` references libaom `aom_roi_map_t` fields
   that don't exist in any released libaom version. SVT-AV1 covers
   production AV1 lane. Re-enabling libaom requires first fixing
   patch 0007's ROI helper to either target real libaom version or
   gate ROI bridge behind version probe.
2. **`libvvenc` (Fraunhofer VVC reference) must be built from
   source, installed under `/usr/local`.** Package not in Ubuntu
   apt. Pin to release tag (`v1.14.0` as of ADR-0568; bumped from
   `v1.12.0` 2026-05-18) so future rebases get deterministic build.
   Configure-time check = `check_pkg_config(libvvenc, ...)`, needs
   `.pc` file `VVENC_ENABLE_INSTALL=ON` ships.
3. **AMF headers vendored from the upstream `GPUOpen-Libraries-
   AndSDKs/AMF` repo (header-only).** FFmpeg's `--enable-amf` needs
   only headers at compile time;
   `libamfrt64.so` runtime resolution = host-side. Do NOT try
   installing `libamfrt64.so` from apt — lives in proprietary
   `amdgpu-pro` userspace, not packaged.
4. **QSV needs both oneVPL dispatcher and GPU runtime.**
   `libvpl-dev` provides `libvpl.so.2`, lets FFmpeg compile
   `--enable-libvpl`, but doesn't provide Gen implementation
   (`libmfx-gen.so`) that creates Arc/iGPU MFX session at runtime.
   `dev/Containerfile` builds `intel/vpl-gpu-rt` at pinned
   `VPL_GPU_RT_TAG`, installs it into `/usr/lib/x86_64-linux-gnu/`,
   path searched by Ubuntu's `libvpl.so.2` dispatcher. Do NOT move
   it back to `/usr/local/lib` without also preserving dispatcher
   discovery, or QSV will regress to
   `Error creating a MFX session: -9` while still appearing in
   `ffmpeg -encoders`.
5. **FFmpeg configure line carries all of `--enable-nvenc
   --enable-cuda-nvcc --enable-libvpl --enable-amf`, in addition to
   software codec flags.** Dropping any one silently disappears
   hardware-encoder family from `ffmpeg -encoders` listing, breaks
   `vmaf-tune compare` sweep. The build-time encoder probe at the end
   of stage 3.5 fails if any promised encoder is missing; listing a
   compiled hardware encoder does not require a device. Do NOT add
   `--enable-libnpp`. FFmpeg n9.0.2 has
   removed libnpp support; the option is a compatibility no-op that
   emits `libnpp has been removed and enabling it does nothing`.
   Keeping it absent preserves warning-clean configure output.
   `scale_cuda` (built via `--enable-cuda-nvcc`) covers the GPU-scale
   pipeline. Reconsider only if a future FFmpeg release restores a
   real libnpp probe and the matching CUDA contract is validated.
   AMF and FFmpeg release checkouts use
   `scripts/ci/checkout-annotated-tag.sh`. Direct `git clone --depth=1
   --branch <tag>` emits a warning for both annotated tags in the image's Git
   version and violates the zero-diagnostic build contract.
6. **FFmpeg SYCL patch must use current libvmaf state-free ownership
   contract.** `libvmaf_sycl.h` declares
   `vmaf_sycl_state_free(VmafSyclState **sycl_state)`, matching
   Vulkan / HIP / Metal rather than CUDA. Keep `ffmpeg-patches/0003-*`
   calling `vmaf_sycl_state_free(&s->sycl_state)`. Passing single
   pointer builds against stale patch text, fails container FFmpeg
   compile with `-Wincompatible-pointer-types`.

### Compose healthcheck invariant (ADR-0641)

`dev-mcp` service healthcheck must match entrypoint transport.
Entrypoint exposes MCP over stdio (`docker exec -i vmaf-dev-mcp
vmafx-mcp` — Go binary, ADR-1229), doesn't create
`/sockets/vmaf-mcp.sock` by default. Compose healthcheck must
therefore remain CLI check (`vmaf --version`). Reverting to
`test -S /sockets/vmaf-mcp.sock` leaves container permanently
`unhealthy`, prevents `smoke-probe-cron` from starting even though
runtime usable.

### Runtime dependency invariants (ADR-0541 / ADR-0568)

1. **`LD_LIBRARY_PATH` must include `${ONEAPI_ROOT}/tbb/latest/lib`
   (ADR-0541).** Intel CPU OpenCL ICD
   (`/opt/intel/oneapi/compiler/latest/lib/libintelocl.so`) dlopens
   `libtbb.so.12` at OpenCL platform-enumeration time. Without
   `tbb/latest/lib`, Khronos ocl-icd loader silently drops Intel CPU
   OpenCL platform, leaving SYCL with no CPU fallback when GPU path
   also degraded. Full env line in Containerfile now
   `${DPCPP_ROOT}/lib:${ONEAPI_ROOT}/umf/latest/lib:${ONEAPI_ROOT}/tcm/latest/lib:${ONEAPI_ROOT}/tbb/latest/lib:${LD_LIBRARY_PATH}`.

2. **NEO + ROCm userspace version-pinned to host kernel's UAPI
   (ADR-0541).** See "Userspace ↔ host-kernel UAPI version pins"
   above. `dev-mcp-entrypoint.sh` banner emits `WARN: SYCL
   level_zero:gpu NOT detected` / `WARN: HIP HSA agent NOT detected`
   line at container start when pin no longer matches host. Bump
   ARG, rebuild instead of working around it on host (CLAUDE.md §12
   r15 sub-rule 4).

3. **`ORT_VERSION` must satisfy `ai/pyproject.toml` requirement
   `onnxruntime>=1.20,<2.0`.** Current pin: `1.26.0` (bumped from
   1.20.1 per ADR-0568 2026-05-18). Tarball naming pattern =
   `onnxruntime-linux-x64-${ORT_VERSION}.tgz` from
   microsoft/onnxruntime GitHub releases. C API stable across 1.x
   line; however, ROCm EP and CUDA EP only available from ORT 1.26+
   (matching container's ROCm 10.0.0 + CUDA 13.x stack —
   ADR-0541/ADR-0542, ROCm bumped by ADR-1225). Bumping ORT_VERSION
   -> verify new version's tarball exists at GitHub releases URL
   before updating ARG, update this note.

## BuildKit cache mount pattern (ADR-0923)

Containerfile uses BuildKit cache mounts to accelerate rebuilds.
Three invariants must hold on every modification:

1. **apt cache mounts pair with no apt-lists cleanup.** Every
   `RUN apt-get install ...` line MUST be prefixed with:

   ```dockerfile
   RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
       --mount=type=cache,target=/var/lib/apt,sharing=locked \
       apt-get update && apt-get install -y --no-install-recommends ...
   ```

   Trailing `&& rm -rf /var/lib/apt/lists/*` cleanup MUST NOT be
   re-added — with cache mount, lists never make it into image
   layer; re-adding cleanup defeats cache.

2. **ccache mount pairs with `CCACHE_DIR=...`.** Every meson / ninja
   / cmake C/C++ compile step MUST be wrapped with ccache cache
   mount plus matching `CCACHE_DIR` env hint. Step running as `vmaf`
   user -> mount needs `uid=2000,gid=2000` (build-deps stage pins
   user identity per ADR-0603); step running as root -> point cache
   at `/root/.cache/ccache`. Shared `id=ccache-dev-mcp` /
   `id=ccache-dev-mcp-vmaf` markers serialise concurrent BuildKit
   workers against same cache, MUST stay consistent across steps
   sharing cache pool. RUN that configures then builds -> export
   `CCACHE_DIR` before both commands; assignment attached only to
   `cd` doesn't reach Meson or Ninja.

3. **`# syntax=docker/dockerfile:1.7`** at top of file enables
   `--mount=type=cache` parsing — do not remove or downgrade
   directive.

4. **`vmaf` user uid/gid pin.** User created with
   `useradd --uid 2000 --gid 2000` in build-deps stage so
   `--mount=...,uid=2000,gid=2000` cache mounts resolve to same
   identity that runs build. Preserve explicit uid/gid pin on any
   modification to user-creation step.

### Source-directory rename sweep invariant (ADR-0966)

After any rename of C source root (currently `core/`, formerly
`libvmaf/` per ADR-0700), run targeted grep across
`dev/Containerfile` before committing:

```bash
grep -n 'COPY.*libvmaf\|cd libvmaf\|/build/vmaf/libvmaf' dev/Containerfile
```

`libvmaf-build` stage name and `libvmaf.so`/`--enable-libvmaf`
occurrences reference **library product name**, must stay unchanged.
Only `COPY`, `cd`, and destination-path occurrences referencing
*source directory* need to track rename.

ADR-0966 fixed three references that survived ADR-0700 rename,
caused `docker compose build dev-mcp` to fail at first COPY step.
Memory rule `feedback_fix_preexisting_bugs_too` (corollary: "Rename
greps must be exhaustive") applies here: single missed grep cost
full build-blockage incident. Run check above as part of any PR
renaming top-level source directory.

## Base images come from `build-config.env` (ADR-1231)

Do not write base image into Dockerfile in this directory. Every
base = `ARG` whose default mirrors root-level `build-config.env`;
edit that file, run `make base-images-sync`, never `ARG` line by
hand.

`COPY --from=<external image>` counts as base-image pin, rejected
with or without digest by
`scripts/ci/check-base-image-single-source.sh`. Declare named stage
instead — `FROM ${CUDA_RUNTIME} AS cuda-runtime-libs`, then
`COPY --from=cuda-runtime-libs …`. BuildKit prunes unused stages, so extra
stage free. Four pins hidden this way were most out-of-date images
in repository.

See [docs/development/base-images.md](../docs/development/base-images.md).
