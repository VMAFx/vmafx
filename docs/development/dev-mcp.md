<!-- markdownlint-disable MD060 -->
# dev-MCP Docker Container

The `dev-MCP` container runs the full VMAF fork inside Docker with all four
GPU backends enabled (CUDA, SYCL, Vulkan, HIP) plus the embedded MCP stdio
server.  It is the standard environment for:

- Live probing of VMAF scores across all backends from a single shell.
- Running the continuous smoke-probe cron (`smoke-probe-cron` service).
- Reproducing build regressions on GPU paths other than the host's primary GPU
  (for example: catching HIP toolchain regressions on an NVIDIA-only host).

The design decision is recorded in [ADR-0435](../adr/0435-local-dev-mcp-container.md).

---

## Prerequisites

### Required

| Component | Version | Notes |
| --- | --- | --- |
| Docker Engine | 26+ | `docker compose` v2 plugin required |
| [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html) | latest | Enables `--gpus all` / `runtime: nvidia` for CUDA kernel execution. The container builds and runs *without* it; CUDA feature extractors return `-ENOSYS` at runtime. |

### Optional

| Component | Purpose |
| --- | --- |
| AMD ROCm runtime on host | Run HIP kernels inside the container. Without it, HIP compiles but returns an error at kernel dispatch. |
| Intel oneAPI runtime on host | Run SYCL kernels via Level Zero. Without it, SYCL falls back to the OpenCL CPU device or returns an error. |
| `jq` | Pretty-print probe JSON output on the host. `apt install jq`. |

---

## How to build

Use the provided wrapper from the repository root:

```bash
./dev/scripts/dev-mcp-up.sh
```

Or, to build without starting:

```bash
docker compose --project-directory "$(pwd)" -f dev/docker-compose.yml build
```

> **Important — always pass `--project-directory`.**  Without it, Docker
> Compose v2 sets the project directory to the compose-file's parent (`dev/`),
> causing `context: .` to resolve to `dev/` instead of the repo root.  This
> bypasses the root `.dockerignore` and — on developer machines that hold
> `.corpus/` (up to 781 GB) — sends the entire corpus into the build context,
> accumulating copies in `/var/lib/docker/overlay2/` on every failed build.
> The `dev-mcp-up.sh` wrapper always passes `--project-directory`; the bare
> `docker compose -f` form is unsafe unless run from the repo root with the
> flag explicit.

The first build downloads all GPU SDK layers and compiles libvmaf from source.
Expect 20–40 minutes on a typical workstation; subsequent builds use the
layer cache and take 1–3 minutes when only Python packages change.

---

## How to start

```bash
# CPU + Vulkan/lavapipe only (no GPU passthrough)
./dev/scripts/dev-mcp-up.sh

# With NVIDIA GPU passthrough
NVIDIA_VISIBLE_DEVICES=all CONTAINER_RUNTIME=nvidia \
    ./dev/scripts/dev-mcp-up.sh
```

To ensure the container joins the correct host `video` and `render` groups
(GIDs differ across distributions), export `HOST_GID_VIDEO` and
`HOST_GID_RENDER` before starting.  The wrapper reads them automatically; you
can also pass them inline:

```bash
HOST_GID_VIDEO=$(getent group video | cut -d: -f3) \
HOST_GID_RENDER=$(getent group render | cut -d: -f3) \
CONTAINER_RUNTIME=nvidia \
docker compose -f dev/docker-compose.yml --project-directory . up -d dev-mcp
```

The defaults baked into `docker-compose.yml` (`44` for `video`, `109` for
`render`) match common Ubuntu installations.  Override whenever `getent group
video` returns a different GID (for example, Arch Linux uses `985`/`986`).

The `dev-mcp-up.sh` wrapper builds (if needed) then starts:

1. `vmaf-dev-mcp` — primary container; runs `vmaf-mcp` via `docker exec -i`
   stdio when requested. The service healthcheck is `vmaf --version`, not a
   socket check.
2. `vmaf-smoke-probe-cron` — waits for the primary to be healthy, then probes
   every 15 minutes.

Both services write probe files to `.workingdir/dev-mcp-probes/` on the host.

---

## How to attach

```bash
# Interactive bash shell inside the running dev-mcp container
./dev/scripts/dev-mcp-shell.sh

# Run a specific command
./dev/scripts/dev-mcp-shell.sh vmaf-dev-mcp vmaf --version
./dev/scripts/dev-mcp-shell.sh vmaf-dev-mcp vmaf --list-features
```

Inside the container the full environment is initialised:

- `vmaf` CLI — `/usr/local/bin/vmaf`
- `vmaf-mcp-server` — `/opt/vmaf-venv/bin/vmaf-mcp-server`
- GPU SDKs — `nvcc`, `icpx`, `hipcc` in `PATH`
- testdata — `/workspace/testdata/` (read-only bind mount from host repo)
- models — `/workspace/model/` (read-only)

---

## How to manually probe

Run a single smoke probe outside the cron cycle:

```bash
./dev/scripts/dev-mcp-probe.sh
```

This executes `smoke-probe-loop.sh --once` inside the running container and
writes `probe-<timestamp>.json` to `.workingdir/dev-mcp-probes/`.  If `jq` is
installed on the host the result is pretty-printed to stdout.

---

## How to stop

```bash
# Stop, keep volumes (probe history preserved)
./dev/scripts/dev-mcp-down.sh

# Stop and remove volumes (clears socket volume; probe bind-mount preserved)
./dev/scripts/dev-mcp-down.sh --volumes
```

---

## How to interpret probe outputs

Each probe file follows this schema:

```json
{
  "ts": "2026-05-15T14:30:00Z",
  "host_id": "myhostname:abc123def456",
  "backend_results": {
    "cpu":    { "score": 76.45, "duration_ms": 3200, "error": null },
    "cuda":   { "score": 76.44, "duration_ms":  820, "error": null },
    "sycl":   { "score": null,  "duration_ms":    0, "error": "ENOSYS: no SYCL device" },
    "vulkan": { "score": 76.45, "duration_ms": 1100, "error": null }
  },
  "mcp_results": {
    "list_features": { "feature_count": 14, "duration_ms": 45, "error": null },
    "compute_vmaf":  { "score": 76.45, "duration_ms": 3250, "error": null }
  }
}
```

| Field | Meaning |
| --- | --- |
| `score` | Aggregate VMAF score for the 48-frame 576×324 golden pair. `null` = backend failed. |
| `duration_ms` | Wall-clock time for the full scoring run. |
| `error` | Error message string, or `null` for success. |
| `feature_count` | Number of features returned by the MCP `list_features` tool. |

### Expected values

- CPU score: ~76.45 (matches the Netflix golden pair; exact value
  varies by model version).
- CUDA / Vulkan scores: within ±0.01 of CPU (numeric parity is not bit-exact —
  see [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)).
- SYCL: `ENOSYS` on hosts without Intel GPU or oneAPI runtime; normal.
- HIP: error on NVIDIA-only hosts; normal.

### Common error patterns

| Error | Cause | Action |
| --- | --- | --- |
| `ENOSYS: no CUDA device` | No NVIDIA GPU or Container Toolkit not installed | Install Container Toolkit and set `NVIDIA_VISIBLE_DEVICES=all` |
| `ENOSYS: no SYCL device` | No Intel GPU / oneAPI runtime | Expected on non-Intel hosts; not a regression |
| `mcp stdio returned empty response` | `vmaf-mcp-server` not in PATH or build failed | Rebuild container; check `docker compose logs dev-mcp` |
| Score drift >0.1 from baseline | Code regression or model change | Run `/validate-scores` skill; check recent commits |

---

## Known limitations

| Limitation | Details |
| --- | --- |
| HIP kernels cannot run on NVIDIA-only hosts | The HIP toolchain in the container compiles and embeds HSACO fat binaries, but the AMD ROCm runtime is not available. Feature extractors return an error at kernel dispatch. The container is still valuable for catching compile-time regressions in HIP paths. |
| Metal is disabled on Linux | `libvmaf` is built with `-Denable_metal=auto`, which resolves to disabled on Linux. Metal kernels require macOS + Apple Silicon. |
| SYCL requires Intel GPU or software emulation | Without the oneAPI Level Zero runtime, SYCL falls back to the OpenCL CPU device (if available) or returns `-ENOSYS`. Performance is significantly lower than on a dedicated Intel GPU. |
| Vulkan lavapipe is CPU-backed | The lavapipe software Vulkan ICD shipped by mesa is enumerated last when no real ICD is available; it allows Vulkan correctness testing without a physical GPU, but throughput is 3–5× slower than real hardware. |
| First build takes 20–40 minutes | All four GPU SDK layers are fetched during `docker compose build`. Subsequent builds are fast (layer cache). |
| `vmaf-tune report` requires matplotlib | Baked into `/opt/vmaf-venv` by `dev/Containerfile` (added 2026-05-18 per ADR-0498). When the container is rebuilt, `vmaf-tune report --format both` produces a self-contained HTML+Markdown report with inline charts. If you see `ModuleNotFoundError: No module named 'matplotlib'` inside the container, your image predates the ADR-0498 commit — `docker compose -f dev/docker-compose.yml build dev-mcp` rebuilds it. |

## Backend matrix (post-ADR-0514)

On a host with NVIDIA + Intel Arc + AMD silicon and the NVIDIA Container
Toolkit installed, every libvmaf backend should run inside the container:

| Backend | Expected | Required host state |
| --- | --- | --- |
| `cpu` | VMAF score, rc=0 | always |
| `cuda` | VMAF score, rc=0 (5-place-equal to CPU per ADR-0214) | NVIDIA GPU + Container Toolkit |
| `sycl` | VMAF score, rc=0 (5-place-equal to CPU) | Intel GPU exposed via `/dev/dri` bind-mount (ADR-0528) |
| `vulkan` | VMAF score, rc=0 (per-adapter device picker selects the first compatible ICD) | At least one Vulkan ICD: NVIDIA (via NVIDIA_DRIVER_CAPABILITIES=graphics), Intel/AMD (via mesa-vulkan-drivers), or lavapipe software fallback |
| `hip` | VMAF score, rc=0 (5-place-equal to CPU) | AMD GPU via `/dev/kfd` + `/dev/dri/renderD*` |
| `metal` | "built without metal support" on Linux containers | macOS host only |

Reproducer:

```bash
docker exec vmaf-dev-mcp bash -c '
  for B in cpu cuda sycl vulkan hip; do
    vmaf --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
         --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
         --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
         --backend $B --json --output /tmp/probe_$B.json
    echo "rc=$? backend=$B"
  done
'
```

### Environment-variable contract

The container pins one env-var family (HSA / ROCm) at compose-up
time, rewrites one (`VK_DRIVER_FILES`) at entrypoint time based on
what is visible on disk, and intentionally leaves everything else
alone. Pinning the wrong subset silently hid one or more GPU
backends in earlier image versions:

| Env var | Contract | Why not pinned |
|---|---|---|
| `VK_ICD_FILENAMES` / `VK_DRIVER_FILES` | unset by default; Vulkan loader uses `/etc/vulkan/icd.d/` + `/usr/share/vulkan/icd.d/` search path | An earlier pin to `lvp_icd.x86_64.json` (typo of `lvp_icd.json`) hid every real GPU. ADR-0509 / Research-0138. |
| `LD_LIBRARY_PATH` | includes `${ONEAPI_ROOT}/{compiler,umf,tcm,tbb}/latest/lib` | `tcm/latest/lib` carries `libhwloc.so.15` (level-zero UR adapter dlopens it at load time; dropping it causes SYCL "Platforms: 0" on Intel Arc). `tbb/latest/lib` carries `libtbb.so.12` (the Intel CPU OpenCL ICD dlopens it at platform enumeration; dropping it silently removes the Intel CPU OpenCL platform — ADR-0543). |
| `NVIDIA_DRIVER_CAPABILITIES` | `compute,graphics,utility,video` (set in `dev/docker-compose.yml` common-env) | `graphics` is what makes the NVIDIA Container Toolkit bind-mount `nvidia_icd.json` into `/etc/vulkan/icd.d/`. Dropping `graphics` hides NVIDIA from Vulkan. |
| Env var | Contract | Rationale |
| --- | --- | --- |
| `VK_DRIVER_FILES` | Rewritten by `dev/scripts/dev-mcp-entrypoint.sh` at container start to the colon-separated list of every non-lavapipe ICD JSON visible under `/etc/vulkan/icd.d/` + `/usr/share/vulkan/icd.d/`. Unset when no real ICD is present (CPU-only fallback). | An earlier image left both env vars unset and relied on alphabetical search order; on multi-vendor hosts where mesa's `lvp_icd.json` sorted before NVIDIA's `nvidia_icd.json` (or Intel/AMD mesa ICDs), `vmaf --vulkan_device 0` silently landed on lavapipe. ADR-0542 closes the race by filtering lavapipe out whenever a real ICD exists. |
| `VK_ICD_FILENAMES` | Unset (deprecated by Khronos in favour of `VK_DRIVER_FILES`). | Setting it overrides the loader's allowlist semantics; the prior `lvp_icd.x86_64.json` typo (ADR-0509 / Research-0138) hid every real GPU. |
| `LD_LIBRARY_PATH` | Includes `${ONEAPI_ROOT}/{compiler,umf,tcm}/latest/lib`. | `tcm/latest/lib` carries `libhwloc.so.15` — the level-zero UR adapter dlopens it at load time. Dropping it causes SYCL "Platforms: 0" on Intel Arc. |
| `NVIDIA_DRIVER_CAPABILITIES` | `compute,graphics,utility,video` (set in `dev/docker-compose.yml` common-env). | `graphics` is what makes the NVIDIA Container Toolkit bind-mount `nvidia_icd.json` into `/etc/vulkan/icd.d/`. Dropping `graphics` hides NVIDIA from Vulkan while leaving CUDA + nvidia-smi working — a hard regression to spot. |
| `HSA_OVERRIDE_GFX_VERSION` | Pinned to `10.3.0` in `common-env`. | AMD `gfx1036` (Raphael iGPU, RDNA2 IP rev 10.3.6) is not on the ROCm 6.x supported-GPU allowlist. Without the override, `hsa_init()` returns `HSA_STATUS_ERROR_OUT_OF_RESOURCES` and `rocminfo` reports "Unable to open /dev/kfd read-write: Invalid argument" even though `/dev/kfd` is bind-mounted. `gfx1036` is binary-compatible enough with `gfx1030` for the libvmaf HIP feature kernels (ADR-0530 / ADR-0538). ADR-0542. |
| `HSA_ENABLE_SDMA` | Pinned to `0` in `common-env`. | On RDNA2 iGPUs sharing system RAM with the CPU, the SDMA copy engine triggers VM faults on small device→host transfers (libvmaf collect path is dominated by such transfers). ADR-0543. |
| `ROCR_VISIBLE_DEVICES` | Pinned to `0` in `common-env`. | Pins HIP to the single AMD adapter on multi-iGPU + dGPU hosts so kernels cannot accidentally dispatch onto a non-RDNA2 device that needs a different `HSA_OVERRIDE_GFX_VERSION`. ADR-0542. |

Operators that need to force a single Vulkan ICD per invocation can
still `docker exec vmaf-dev-mcp env VK_DRIVER_FILES=/path/to/icd.json
vmaf …` — the per-exec env var overrides the entrypoint-time pin.
Operators on hosts with a ROCm-supported GPU on the allowlist
(`gfx1030` / `gfx1100` / `gfx1101` desktop / workstation parts) can
override `HSA_OVERRIDE_GFX_VERSION` to the empty string at
`docker compose up` time to remove the lie.

## FFmpeg encoder matrix (post-ADR-0543)

The in-image FFmpeg is built with the fork's full encoder set so that
`vmaf-tune compare` sweeps can address every codec the project
supports without skipping rows with `hardware encoder not available:
... not compiled into ffmpeg`. The matrix:

| Encoder | Compile-in source | Host runtime requirement |
| --- | --- | --- |
| `libx264` | `libx264-dev` (apt) | none |
| `libx265` | `libx265-dev` (apt) | none |
| `libvpx-vp9` | `libvpx-dev` (apt) | none |
| `libsvtav1` | source build (SVT-AV1 pinned in `dev/Containerfile`) | none |
| `libaom-av1` | adapter exists, but the in-image FFmpeg intentionally omits libaom until patch 0007's ROI bridge targets released libaom fields | external FFmpeg with `--enable-libaom`, or wait for the patch-stack follow-up |
| `libvvenc` | source build (Fraunhofer VVenC v1.14.0) | none |
| `h264_nvenc` / `hevc_nvenc` / `av1_nvenc` | `--enable-nvenc` + `nv-codec-headers` | NVIDIA GPU + Container Toolkit; NVENC capability bit on host driver (av1_nvenc requires Ada or newer — RTX 4090 ok) |
| `h264_qsv` / `hevc_qsv` / `av1_qsv` | `--enable-libvpl` + `libvpl-dev` dispatcher + pinned `intel/vpl-gpu-rt` (`libmfx-gen.so`) source build installed under `/usr/lib/x86_64-linux-gnu/` | Intel GPU + `/dev/dri/renderD*` passthrough; the container auto-selects the Intel render node for QSV |
| `h264_amf` / `hevc_amf` / `av1_amf` | `--enable-amf` + AMF headers (source) | AMD GPU + `libamfrt64.so` from the proprietary `amdgpu-pro` userspace bind-mounted into the container. The open-source ROCm install in the image (`rocm-hip-runtime-dev`) does **not** include AMF. |

To verify the in-image listing after a rebuild:

```bash
docker exec vmaf-dev-mcp ffmpeg -hide_banner -encoders 2>&1 \
    | grep -E "libsvtav1|libvvenc|libvpx-vp9|nvenc|qsv|amf|vpl" \
    | head -20
```

Expected (assuming the build-time encoder probe in stage 3.5 logged no
`WARN ... missing`):

```text
 V....D libsvtav1            SVT-AV1(Scalable Video Technology for AV1) encoder
 V..... libvvenc             libvvenc-based VVC encoder
 V....D libvpx-vp9           libvpx VP9
 V....D h264_nvenc           NVIDIA NVENC H.264 encoder
 V....D hevc_nvenc           NVIDIA NVENC hevc encoder
 V....D av1_nvenc            NVIDIA NVENC av1 encoder
 V....D h264_qsv             H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (Intel Quick Sync Video acceleration)
 V....D hevc_qsv             HEVC (Intel Quick Sync Video acceleration)
 V....D av1_qsv              AV1 (Intel Quick Sync Video acceleration)
 V....D h264_amf             AMD AMF H.264 Encoder
 V....D hevc_amf             AMD AMF HEVC encoder
 V....D av1_amf              AMD AMF AV1 encoder
```

### Hardware-encoder runtime failure modes

The encoders above are split into "compile-in" (does the binary
advertise the encoder?) and "runtime-ok" (does a 1-frame dummy encode
succeed?). `vmaf-tune compare`'s
`compare.py::probe_encoder_available` runs both stages. The container
locks down the compile-in promise; runtime failures produce stable
row-level skip strings:

| Symptom | Cause | Action |
| --- | --- | --- |
| `hardware encoder not available: h264_nvenc dummy encode failed (...): Cannot load libcuda.so.1` | Container started without `runtime: nvidia` | `CONTAINER_RUNTIME=nvidia ./dev/scripts/dev-mcp-up.sh` |
| `hardware encoder not available: av1_nvenc dummy encode failed: Cannot load library` | Host NVIDIA driver too old for AV1 NVENC (Turing/Ampere don't have av1_nvenc) | Use h264_nvenc / hevc_nvenc on that host; av1_nvenc needs Ada or newer |
| `hardware encoder not available: h264_qsv dummy encode failed: Error creating a MFX session` | Stale image missing `libmfx-gen.so` in the dispatcher search path, or Intel iGPU not exposed (`/dev/dri/renderD*` missing) | Rebuild `dev-mcp` so the pinned `intel/vpl-gpu-rt` layer is present under `/usr/lib/x86_64-linux-gnu/`; verify `vainfo --display drm --device /dev/dri/renderD<N>` on the Intel node |
| `hardware encoder not available: h264_amf dummy encode failed: ... cannot open shared object libamfrt64.so` | amdgpu-pro userspace not bind-mounted | Install `amdgpu-pro` on the host and bind-mount `/opt/amdgpu-pro/lib/x86_64-linux-gnu/libamfrt64.so` into the container, or accept that AMF encode is unavailable on this host |

### Reproducer — full cross-codec compare sweep

```bash
docker exec vmaf-dev-mcp bash -c '
  cd /workspace && PYTHONPATH=/workspace/tools/vmaf-tune/src:$PYTHONPATH \
  python -c "from vmaftune.cli import main; raise SystemExit(main())" compare \
    --src /workspace/.corpus/bbb_e2e/bbb_sunflower_1080p_60fps_normal.mp4 \
    --width 1920 --height 1080 --framerate 60 \
    --target-vmafs 85,90,92,95 \
    --encoders libx264,libx265,libsvtav1,libaom-av1,libvvenc,libvpx-vp9,h264_nvenc,hevc_nvenc,av1_nvenc,h264_qsv,hevc_qsv,av1_qsv,h264_amf,hevc_amf,av1_amf \
    --duration 5 --sample-clip-seconds 3 --max-iterations 3 \
    --score-backend cuda --format json --output /tmp/v11_1080p_cmp_full.json'
```

Encoders that are not runtime-available on the host produce per-row
`ok=false` entries with the diagnostic strings above; the sweep does
not abort.

### Host-kernel ↔ container-userspace UAPI version pins (ADR-0543)

Intel NEO compute-runtime and ROCm KFD userspace are version-pinned via
Containerfile ARGs to match the host kernel's i915 / xe / KFD ioctl ABI.
A mismatch silently degrades `vmaf --backend sycl|hip` to CPU.

| Pin | Current value | Why pinned |
|---|---|---|
| `ARG NEO_VER` | `26.31.39395.13` | Intel's `noble/unified` APT repo's newest as of 2026-05-18 is `25.18.x`, too old for kernel ≥ 7.0. NEO 25.18 returns `ZE_RESULT_ERROR_UNINITIALIZED` from `zeInit()` against kernel-7.x i915/xe. Pulled from `github.com/intel/compute-runtime/releases`. |
| `ARG IGC_VER` + `ARG GMMLIB_VER` | `2.40.13+22418` + `22.10.0` | NEO 26.31's release notes mandate IGC v2.40.13 + gmmlib 22.10.0. Pinned together. |
| `ARG ROCM_VER` | `7.2.4` | Matches Arch host `hsa-rocr 7.2.4`. ROCm 6.x KFD userspace returns `Unable to open /dev/kfd read-write: Invalid argument` against kernel-7.x KFD ioctls. |

`dev-mcp-entrypoint.sh` emits a runtime visibility probe on container
start (ADR-0543): `WARN: SYCL level_zero:gpu NOT detected` or `WARN: HIP
HSA agent NOT detected` means the host kernel has revved past the pinned
userspace ABI — bump the ARG and rebuild rather than working around the
fallback (CLAUDE.md §12 r15 sub-rule 4). The latest NEO release tag is at
`https://github.com/intel/compute-runtime/releases/latest`; the latest
ROCm noble channel is listed under
`https://repo.radeon.com/rocm/apt/`.
