<!-- markdownlint-disable MD060 -->
# dev-MCP Docker Container

The `dev-MCP` container runs the full VMAF fork inside Docker with the three
active Linux GPU backends enabled (CUDA, SYCL, HIP), the CPU backend, and the
production Go MCP stdio server (`vmafx-mcp`). Metal remains a macOS-only
backend. It is the standard environment for:

- Live probing of VMAF scores across all backends from a single shell.
- Running the continuous smoke-probe cron (`smoke-probe-cron` service).
- Reproducing build regressions on GPU paths other than the host's primary GPU
  (for example: catching HIP toolchain regressions on an NVIDIA-only host).

The design decision is recorded in
[ADR-0451](../adr/0451-local-dev-mcp-container.md). The agent rule that makes
this container the default for vmaf, vmaf-tune, ai and MCP work is hard rule 12
of [agent-hard-rules.md](agent-hard-rules.md).

!!! note
    Which MCP server runs where: this container carries the Go server
    `vmafx-mcp` (built from `cmd/vmafx-mcp`), which
    [ADR-1229](../adr/1229-mcp-go-runtime.md) made the MCP server everywhere
    and which replaces the Python `mcp-server/vmaf-mcp` package (deprecated,
    not deleted). The published `-server` production image is the exception:
    its entry point is still the Python `vmaf-mcp --transport http`. See
    [docker-production.md](docker-production.md).

## Prerequisites

### Required

| Component | Version | Notes |
| --- | --- | --- |
| Docker Engine | 26+ | `docker compose` v2 plugin required; the published `vmafx-dev-mcp` image has zstd layers, which need 23.0 or later ([ADR-1594](../adr/1594-zstd-images-zopfli-zips.md)) |
| [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html) | latest | Enables `--gpus all` / `runtime: nvidia` for CUDA kernel execution. The container builds and runs *without* it; CUDA feature extractors return `-ENOSYS` at runtime. |

### Optional

| Component | Purpose |
| --- | --- |
| AMD ROCm runtime on host | Run HIP kernels inside the container. Without it, HIP compiles but returns an error at kernel dispatch. |
| Intel oneAPI runtime on host | Run SYCL kernels via Level Zero. Without it, SYCL falls back to the OpenCL CPU device or returns an error. |
| `jq` | Pretty-print probe JSON output on the host. `apt install jq`. |

## How to start

Build (if needed) and start the container with the wrapper from the
repository root:

```bash
# CPU only (no GPU passthrough)
./dev/scripts/dev-mcp-up.sh

# With NVIDIA GPU passthrough
NVIDIA_VISIBLE_DEVICES=all CONTAINER_RUNTIME=nvidia \
    ./dev/scripts/dev-mcp-up.sh
```

The first build downloads all GPU SDK layers and compiles libvmaf from source.
Expect 20-40 minutes on a typical workstation; later builds use the layer
cache and take 1-3 minutes when only Python packages change. To rebuild
without starting, see [Building and maintaining the
image](#building-and-maintaining-the-image).

The wrapper starts two services:

1. `vmaf-dev-mcp` - the primary container. It runs `vmafx-mcp` through
   `docker exec -i` stdio when requested. The service healthcheck verifies
   `vmaf --version` and, when `/dev/nvidia0` is exposed, requires a successful
   `nvidia-smi` driver query. It is not a socket check. A 45-second start
   period covers CUDA driver cold-start before dependent services are
   admitted.
2. `vmaf-smoke-probe-cron` - waits for the primary to be healthy, then probes
   every 15 minutes.

Both services write probe files to `.workingdir/dev-mcp-probes/` on the host.

### Host `video` and `render` groups

Group IDs differ across distributions, so the container must join the host's
`video` and `render` groups. Export `HOST_GID_VIDEO` and `HOST_GID_RENDER`
before starting; the wrapper reads them automatically. You can also pass them
inline:

```bash
HOST_GID_VIDEO=$(getent group video | cut -d: -f3) \
HOST_GID_RENDER=$(getent group render | cut -d: -f3) \
CONTAINER_RUNTIME=nvidia \
docker compose -f dev/docker-compose.yml --project-directory . up -d dev-mcp
```

The defaults baked into `docker-compose.yml` (`44` for `video`, `109` for
`render`) match common Ubuntu installations. Override them whenever
`getent group video` returns a different GID (for example, Arch Linux uses
`985`/`986`).

## How to attach

```bash
# Interactive bash shell inside the running dev-mcp container
./dev/scripts/dev-mcp-shell.sh

# Run a specific command
./dev/scripts/dev-mcp-shell.sh vmaf-dev-mcp vmaf --version
./dev/scripts/dev-mcp-shell.sh vmaf-dev-mcp vmaf --help
```

### One-shot commands

Started without a command, the image stays up so you can `docker exec` into it.
Started with a command, it runs that command and exits with its status:

```bash
# A throwaway probe: runs, prints, exits, and the container is removed
docker run --rm vmaf-dev-mcp:local pkg-config --modversion vpl

# The same against the running dev-mcp container
docker exec vmaf-dev-mcp clang-tidy --version
```

Use `docker run --rm` (or `docker exec vmaf-dev-mcp`, agent hard rule 12) for
one-shot work. Before this behaviour every one-shot `docker run` left a running
container with its healthcheck, because the entrypoint ignored its arguments.

Inside the container the full environment is initialised:

- `vmaf` CLI - `/usr/local/bin/vmaf`
- `vmafx-mcp` - `/usr/local/bin/vmafx-mcp` (Go; ADR-1229 replaced the
  Python `mcp-server/vmaf-mcp` package, which is deprecated)
- GPU SDKs - `nvcc`, `icpx`, `hipcc` in `PATH`
- testdata - `/workspace/testdata/` (read-only bind mount from host repo)
- models - `/workspace/model/` (read-only)
- corpus - `/workspace/.corpus/` (read-only, its own bind mount; see below)

### Corpus mount

Both services mount the corpus separately from the repository. The source is
`VMAFX_CORPUS_DIR`, or `./.corpus` in the repository root when it is unset:

```bash
VMAFX_CORPUS_DIR=/srv/corpus/vmafx ./dev/scripts/dev-mcp-up.sh
```

- `.corpus` may be a symlink to a dataset elsewhere on the host. The
  repository bind alone shows the container a link to a path it does not have,
  so `ls /workspace/.corpus` fails. Docker resolves the separate bind's source
  on the host, and the link inside the container then leads to the mounted
  corpus.
- On a host without a corpus, Compose creates an empty `.corpus` directory
  (owned by root) and the services start.
- The image runs as `vmaf` (uid 2000). That user must be able to enter the
  repository root and read the corpus. A repository root with mode `0700`
  makes all of `/workspace` unreadable inside the container, the corpus
  included.
- The build context still excludes the corpus (`.dockerignore`); the mount
  exists only at run time.

To list the registered feature extractors, call the MCP tool
`list_extractors` (the manual probe below does this for you).

## How to manually probe

Run a single smoke probe outside the cron cycle:

```bash
./dev/scripts/dev-mcp-probe.sh
```

This executes `smoke-probe-loop.sh --once` inside the running container and
writes `probe-<timestamp>.json` to `.workingdir/dev-mcp-probes/`. If `jq` is
installed on the host the result is pretty-printed to stdout.

## How to stop

```bash
# Stop, keep volumes (probe history preserved)
./dev/scripts/dev-mcp-down.sh

# Stop and remove volumes (clears socket volume; probe bind-mount preserved)
./dev/scripts/dev-mcp-down.sh --volumes
```

## How to interpret probe outputs

Each probe file follows this schema:

```json
{
  "ts": "2026-05-15T14:30:00Z",
  "host_id": "myhostname:abc123def456",
  "backend_results": {
    "cpu":  { "score": 94.32301, "duration_ms": 3200, "error": null },
    "cuda": { "score": 94.32301, "duration_ms":  820, "error": null },
    "sycl":   { "score": null,  "duration_ms":    0, "error": "ENOSYS: no SYCL device" },
    "hip":    { "score": null,  "duration_ms":    0, "error": "ENOSYS: no HIP device" }
  },
  "mcp_results": {
    "list_features": { "feature_count": 14, "duration_ms": 45, "error": null },
    "compute_vmaf":  { "score": 94.32301, "duration_ms": 3250, "error": null }
  }
}
```

| Field | Meaning |
| --- | --- |
| `score` | Pooled VMAF score (model `vmaf_v0.6.1`) for the 48-frame 576x324 pair `testdata/ref_576x324_48f.yuv` / `dis_576x324_48f.yuv`. `null` = backend failed. |
| `duration_ms` | Wall-clock time for the full scoring run. |
| `error` | Error message string, or `null` for success. |
| `feature_count` | Number of extractors returned by the Go MCP `list_extractors` tool. |

The `list_features` and `compute_vmaf` names in the probe JSON are stable
schema keys retained for existing probe consumers. The operations behind them
are the current Go MCP tools `list_extractors` and `vmaf_score`, respectively.
Both MCP probes and every backend probe score the same pair with the same
model, so `compute_vmaf` carries the CPU score.

Each MCP probe performs the required `initialize` /
`notifications/initialized` handshake before its `tools/call` request and
keeps the server's stdin open until the matching response arrives. Closing
stdin after writing the request disconnects the Go SDK session and can race the
asynchronous response.

### Expected values

- CPU score: 94.32301 for the committed 48-frame pair with `vmaf_v0.6.1`
  (checked against a CPU build of master).
- CUDA / SYCL / HIP scores: equal to the CPU's bits for every extractor
  declared an exact twin (one file per twin in `scripts/ci/exact_twins.d/`,
  listed in the [generated table](cross-backend-exact-twins.md)). A feature
  that is not declared exact stays inside the tolerance of the cross-backend
  gate (for example `ciede` at `1e-9`); see
  [cross-backend-gate.md](cross-backend-gate.md) and
  [ADR-0214](../adr/0214-gpu-parity-ci-gate.md).
- SYCL: `ENOSYS` on hosts without Intel GPU or oneAPI runtime; normal.
- HIP: error on NVIDIA-only hosts; normal.

### Common error patterns

| Error | Cause | Action |
| --- | --- | --- |
| `ENOSYS: no CUDA device` | No NVIDIA GPU or Container Toolkit not installed | Install Container Toolkit and set `NVIDIA_VISIBLE_DEVICES=all` |
| `ENOSYS: no SYCL device` | No Intel GPU / oneAPI runtime | Expected on non-Intel hosts; not a regression |
| `mcp stdio returned empty response` | `vmafx-mcp` missing/failed, or a custom client closed stdin before the response | Rebuild and check `docker compose logs dev-mcp`; custom clients must keep stdin open through the matching response. The binary comes from `cmd/vmafx-mcp` via the `go-build` stage, not from the venv. |
| Score drift >0.1 from baseline | Code regression or model change | Run `/validate-scores` skill; check recent commits |

## Known limitations

| Limitation | Details |
| --- | --- |
| HIP kernels cannot run on NVIDIA-only hosts | The HIP toolchain compiles and embeds HSACO fat binaries, but the AMD ROCm runtime is not available, so feature extractors return an error at kernel dispatch. The container still catches compile-time regressions in HIP paths. |
| Metal is disabled on Linux | `libvmaf` is built with `-Denable_metal=auto`, which resolves to disabled on Linux. Metal kernels require macOS and Apple Silicon. |
| SYCL requires Intel GPU or software emulation | Without the oneAPI Level Zero runtime, SYCL falls back to the OpenCL CPU device (if available) or returns `-ENOSYS`. Performance is significantly lower than on a dedicated Intel GPU. |
| First build takes 20-40 minutes | All three Linux GPU SDK layers are fetched during `docker compose build`. Later builds are fast (layer cache). |
| `vmaf-tune report` requires matplotlib | Baked into `/opt/vmaf-venv` by `dev/Containerfile` (ADR-0498). Details below the table. |

For the `vmaf-tune report` row: after a rebuild, `vmaf-tune report --format
both` produces a self-contained HTML and Markdown report with inline charts.
If you see `ModuleNotFoundError: No module named 'matplotlib'` inside the
container, the image predates the ADR-0498 commit (2026-05-18); rebuild it with
`docker compose -f dev/docker-compose.yml build dev-mcp`.

## Backend matrix

On a host with NVIDIA, Intel Arc and AMD silicon and the NVIDIA Container
Toolkit installed, every libvmaf backend should run inside the container:

| Backend | Expected | Required host state |
| --- | --- | --- |
| `cpu` | VMAF score, rc=0 | always |
| `cuda` | VMAF score, rc=0 (equal to CPU for exact twins, see [Expected values](#expected-values)) | NVIDIA GPU + Container Toolkit |
| `sycl` | VMAF score, rc=0 (same) | Intel GPU exposed via `/dev/dri` bind-mount (ADR-0528) |
| `hip` | VMAF score, rc=0 (same) | AMD GPU via `/dev/kfd` + `/dev/dri/renderD*` |
| `metal` | "built without metal support" on Linux containers | macOS host only |

Reproducer:

```bash
docker exec vmaf-dev-mcp bash -c '
  for B in cpu cuda sycl hip; do
    vmaf --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
         --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
         --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
         --backend $B --json --output /tmp/probe_$B.json
    echo "rc=$? backend=$B"
  done
'
```

### Environment-variable contract

The container pins the ROCm variables at compose-up time and carries the
oneAPI runtime libraries needed for SYCL discovery. The VMAFx Vulkan backend
was removed by ADR-0726 and is not part of this contract; the Vulkan loader the
image carries for the frame-import tests needs no variable (see
[Vulkan loader and lavapipe](#vulkan-loader-and-lavapipe)).

| Env var | Contract | Rationale |
| --- | --- | --- |
| `LD_LIBRARY_PATH` | Includes `${ONEAPI_ROOT}/{compiler,umf,tcm,tbb}/latest/lib`. | `tcm/latest/lib` carries `libhwloc.so.15` for the Level Zero adapter; `tbb/latest/lib` carries `libtbb.so.12` for the Intel CPU OpenCL ICD. Dropping either can silently remove a SYCL platform. |
| `HSA_OVERRIDE_GFX_VERSION` | **Removed** (was `10.3.0`). | ROCm 10 supports `gfx1036` natively, so the alias is not merely unnecessary but wrong: it would map the agent to `gfx1030` while meson compiles `gfx1036` code objects. ADR-0542, superseded on this point by ADR-1225. History below. |
| `HSA_ENABLE_SDMA` | Pinned to `0` in `common-env`. | On RDNA2 iGPUs sharing system RAM with the CPU, the SDMA copy engine triggers VM faults on small device-to-host transfers (libvmaf's collect path is dominated by such transfers). ADR-0543. |
| `ROCR_VISIBLE_DEVICES` | Pinned to `0` in `common-env`. | Pins HIP to the single AMD adapter on multi-iGPU + dGPU hosts so kernels cannot dispatch onto a non-RDNA2 device that needs a different `HSA_OVERRIDE_GFX_VERSION`. ADR-0542. |

Operators on hosts with a ROCm-supported GPU on the allowlist
(`gfx1030` / `gfx1100` / `gfx1101` desktop / workstation parts) can
override `HSA_OVERRIDE_GFX_VERSION` to the empty string at
`docker compose up` time to remove the lie.

## FFmpeg encoder matrix

The in-image FFmpeg is built with the fork's full encoder set so that
`vmaf-tune compare` sweeps can address every codec the project supports
without skipping rows with `hardware encoder not available: ... not compiled
into ffmpeg` (ADR-0543). The matrix:

| Encoder | Compile-in source | Host runtime requirement |
| --- | --- | --- |
| `libx264` | `libx264-dev` (apt) | none |
| `libx265` | `libx265-dev` (apt) | none |
| `libvpx-vp9` | `libvpx-dev` (apt) | none |
| `libsvtav1` | source build (SVT-AV1 pinned in `dev/Containerfile`) | none |
| `libaom-av1` | adapter exists, but the in-image FFmpeg intentionally omits libaom until patch 0007's ROI bridge targets released libaom fields | external FFmpeg with `--enable-libaom`, or wait for the patch-stack follow-up |
| `libvvenc` | source build (Fraunhofer VVenC v1.14.0) | none |
| `h264_nvenc` / `hevc_nvenc` / `av1_nvenc` | `--enable-nvenc` + `nv-codec-headers` | NVIDIA GPU + Container Toolkit; NVENC capability bit on host driver (`av1_nvenc` requires Ada or newer; RTX 4090 is fine) |
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

Each encoder has two checks. "Compile-in" asks whether the binary advertises
the encoder. "Runtime-ok" asks whether a 1-frame dummy encode succeeds.
`vmaf-tune compare` runs both in `compare.py::probe_encoder_available`. The
container locks down the compile-in promise; runtime failures produce stable
row-level skip strings:

| Symptom | Cause | Action |
| --- | --- | --- |
| `hardware encoder not available: h264_nvenc dummy encode failed (...): Cannot load libcuda.so.1` | Container started without `runtime: nvidia` | `CONTAINER_RUNTIME=nvidia ./dev/scripts/dev-mcp-up.sh` |
| `hardware encoder not available: av1_nvenc dummy encode failed: Cannot load library` | Host NVIDIA driver or GPU too old for AV1 NVENC (Turing and Ampere have no `av1_nvenc`) | Use `h264_nvenc` / `hevc_nvenc` on that host; `av1_nvenc` needs Ada or newer |
| `hardware encoder not available: h264_qsv dummy encode failed: Error creating a MFX session` | Stale image missing `libmfx-gen.so` in the dispatcher search path, or Intel iGPU not exposed (`/dev/dri/renderD*` missing) | Rebuild `dev-mcp` so the pinned `intel/vpl-gpu-rt` layer is present under `/usr/lib/x86_64-linux-gnu/`; verify `vainfo --display drm --device /dev/dri/renderD<N>` on the Intel node |
| `hardware encoder not available: h264_amf dummy encode failed: ... cannot open shared object libamfrt64.so` | `amdgpu-pro` userspace not bind-mounted | Install `amdgpu-pro` on the host and bind-mount `/opt/amdgpu-pro/lib/x86_64-linux-gnu/libamfrt64.so` into the container, or accept that AMF encode is unavailable on this host |

### Reproducer: full cross-codec compare sweep

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
`ok=false` entries with the diagnostic strings above; the sweep does not
abort.

## Building and maintaining the image

This section is for anyone rebuilding the container or changing
`dev/Containerfile`. Day-to-day use needs only the sections above.

### Rebuild the image

Use the wrapper from the repository root:

```bash
./dev/scripts/container-build.sh
```

Prefer it over a bare `docker compose build`. Before spending 20-40 minutes it
checks what it is about to build from, and it records the answer in the image
([ADR-1195](../adr/1195-container-source-revision-guard.md)). The bare
compose form still works and is what the wrapper calls underneath:

```bash
docker compose --project-directory "$(pwd)" -f dev/docker-compose.yml build
```

It performs no source check and leaves the image recording
`source_rev=unknown`.

!!! warning
    Always pass `--project-directory`. Without it, Docker Compose v2 sets the
    project directory to the compose file's parent (`dev/`), so `context: .`
    resolves to `dev/` instead of the repo root. That bypasses the root
    `.dockerignore`; on developer machines that hold `.corpus/` (up to 781 GB)
    the entire corpus is sent into the build context, and copies accumulate in
    `/var/lib/docker/overlay2/` on every failed build. The `dev-mcp-up.sh`
    wrapper always passes the flag; the bare `docker compose -f` form is
    unsafe unless run from the repo root with the flag explicit.

### Which source is in the image?

A rebuild only picks up work that is in the checkout you build from. If the
checkout is behind `master`, the build still succeeds and the image is still
newer than every commit in the repository, but it does not contain the commits
you rebuilt for. Timestamps cannot tell the two cases apart, so ask the image
directly (the incident that motivated this check is under [History](#history)):

```bash
# Is this checkout a valid build context right now?
bash scripts/dev/check-container-source.sh --pre-build

# What was an existing image actually built from?
bash scripts/dev/check-container-source.sh --image vmaf-dev-mcp:local
```

Exit codes:

| Code | Meaning |
| --- | --- |
| `0` | Current. |
| `1` | Stale; the missing commits are listed. |
| `2` | Cannot tell: the image predates the marker, or was built by a bare `docker compose build` that never received `VMAFX_SOURCE_REV`. |

Treat `2` as "rebuild before trusting anything measured in here", not as a
pass. Before citing a number produced inside the container (a GPU smoke, a
benchmark, a parity sweep), check the image first: an unattributable
measurement is worse than none, because it looks like evidence.

### Optional GitHub API authentication

The Intel NEO resolver works without credentials. On a shared network, an
authenticated GitHub API request can avoid the lower anonymous rate limit.
For Compose and the wrapper scripts, export `GITHUB_TOKEN` before the build;
`dev/docker-compose.yml` maps it to the optional `github_token` build secret.
An unset or empty value keeps the build anonymous.

For a raw anonymous build, omit the secret entirely:

```bash
env -u GITHUB_TOKEN docker build \
  --file dev/Containerfile \
  --target libvmaf-build \
  --tag vmaf-dev-mcp:local \
  .
```

For an authenticated raw build, pass the exported variable as a BuildKit
secret:

```bash
docker build \
  --file dev/Containerfile \
  --target libvmaf-build \
  --tag vmaf-dev-mcp:local \
  --secret id=github_token,env=GITHUB_TOKEN \
  .
```

Never pass this credential with `--build-arg`. The secret is mounted only for
the NEO metadata-fetch instruction and is not recorded in image layers,
metadata, or provenance. See
[ADR-1271](../adr/1271-neo-buildkit-github-token-secret.md).

### CI stage builds

CI builds stage images through one script,
`scripts/ci/build-dev-container-stage.sh <target> <image-tag>`. It accepts two
targets, uses no external layer cache and passes no build arguments:

| Target | Used by | Notes |
| --- | --- | --- |
| `libvmaf-build` | The Dev Container PR gate, which builds and smoke-tests it | The script forwards the optional `github_token` secret; an unset `GITHUB_TOKEN` keeps the build anonymous. |
| `release-build` | The native release job (`build-artifacts` in `supply-chain.yml`), rehearsed by the PR gate on every container-affecting pull request ([ADR-1354](../adr/1354-native-bundle-release-track.md)) | A small stage on the Debian 13 release-track base (`RELEASE_BUILDER_BASE` in `build-config.env`) with only the compiler and build tools. It fetches nothing from GitHub, so the script never passes it a secret. |

The script no longer accepts `build-deps`: no CI job builds that stage on its
own. Run it to reproduce either job's image:

```bash
bash scripts/ci/build-dev-container-stage.sh libvmaf-build vmaf-dev-mcp:local
bash scripts/ci/build-dev-container-stage.sh release-build vmafx-release-build:local
```

See [publishing](publishing.md#release-compilation-environment-adr-1346) for
the release build that runs inside `release-build`.

CI builds the image only up to `libvmaf-build`
([ADR-0819](../adr/0819-dev-container-ci-gate.md)), plus `release-build` on
its own for the release rehearsal
([ADR-1346](../adr/1346-hosted-slim-container-release-build.md)). The final
`dev-mcp` stage is therefore covered by a static contract instead
([ADR-1343](../adr/1343-dev-container-stage-input-contract.md)):

```bash
python3 scripts/ci/check-dev-container-stage-inputs.py
```

The check fails in two cases:

- A stage's `RUN` reads a pip requirement or constraint file under
  `/build/vmaf/` that no `COPY` into that stage or its parent stages provides.
- A `COPY` source is missing from the repository.

It runs from pre-commit whenever `dev/Containerfile`, a lock file or the check
itself changes. Fix a failure by adding the matching `COPY` to the stage that
reads the file; copying it in the final stage keeps the libvmaf and FFmpeg
layers cached.

### Compiler selection

The stages use two GCC versions:

- In `build-deps`, `gcc`, `g++`, `gcc-ar`, `gcc-nm` and `gcc-ranlib` all point
  at GCC 13 through `update-alternatives`.
- A later stage installs Ubuntu's default GCC 15, which replaces those links,
  so `libvmaf-build` and the final image compile with GCC 15.
- `release-build` installs Debian's default `gcc` package, which provides
  `gcc-ar` and the LTO linker plugin itself.

The `gcc-ar` link matters in `build-deps`. Meson archives static libraries with
`gcc-ar` when it exists; without it, plain `ar` cannot index GCC's LTO objects
in that stage and every LTO link against `libvmaf.a` fails with undefined
references.

### Build failures and cache use

- Every stage that runs build pipelines explicitly enables Bash `pipefail`, so
  an upstream command failure cannot be hidden by a successful output filter.
- The golden-suite import and collection checks fail the build; collection
  failures print the captured pytest diagnostics.
- Hardware-availability probes keep their documented warning behaviour in a
  build sandbox without GPUs.
- The libvmaf configure and compile commands share the exported `CCACHE_DIR`
  backed by the BuildKit cache mount.
- Install steps address their build trees explicitly, and FFmpeg cleanup runs
  from outside the directory it removes.
- The Go artifact stage verifies seven outputs without parsing filenames as
  lines, then returns to the `vmaf` user. The final runtime also uses `vmaf`.

Run `hadolint dev/Containerfile` to check the Dockerfile and embedded shell
before a rebuild. This static check does not establish native build or GPU
runtime acceptance.

### ONNX Runtime in the image

The container installs ONNX Runtime's native CPU archive for libvmaf's C/C++
API. That archive does not add CUDA or ROCm execution providers; libvmaf's own
GPU feature backends and the Python `onnxruntime` package are separate
components. A provider-enabled ORT installation needs the corresponding
runtime libraries, as described in
[ADR-0113](../adr/0113-ort-create-session-fallback-multi-ep-ci.md).

### Host-kernel and container-userspace version pins

Intel NEO compute-runtime and ROCm KFD userspace are version-pinned through
Containerfile ARGs to match the host kernel's i915 / xe / KFD ioctl ABI
(ADR-0543). A mismatch silently degrades `vmaf --backend sycl|hip` to CPU.

| Pin | Current value | Why pinned |
| --- | --- | --- |
| `INTEL_NEO_VERSION` | Defined in [`build-config.env`](../../build-config.env) | Intel's `noble/unified` APT repo's newest as of 2026-05-18 is `25.18.x`, too old for kernel 7.0 and later: NEO 25.18 returns `ZE_RESULT_ERROR_UNINITIALIZED` from `zeInit()` against kernel-7.x i915/xe. Pulled from `github.com/intel/compute-runtime/releases`. The matching `gmmlib`, `IGC` and `intel-ocloc` deb packages and checksums are derived at build time by `dev/scripts/fetch-intel-neo.py` (ADR-1145). `ocloc` is the offline compiler icpx runs for the SYCL ahead-of-time images; CI build hosts install the same release's copy with `scripts/ci/install-intel-ocloc.sh` (ADR-1360). |
| `rocm-src` stage image | `rocm/dev-ubuntu-26.04:10.1.0-full` (digest-pinned; `ROCM_BUILDER` in `build-config.env` and `dev/Containerfile`) | Replaces the old `ARG ROCM_VER` apt install: ROCm 7.14 and later ship only as a container image (ADR-1225). ROCm 6.x KFD userspace returns `Unable to open /dev/kfd read-write: Invalid argument` against kernel-7.x KFD ioctls; 10.0.0 was verified against Linux 7.2.3 on `gfx1036` and 10.1.0 against Linux 7.2.9. The stage prunes about 15 GB of math libraries libvmaf never links (21 GB to 5.8 GB in 10.1.0), but never `librocprofiler-register`, which `libamdhip64.so` needs at load. |

The NEO resolver accepts only GitHub-hosted HTTPS URLs. API credentials are
sent only to `api.github.com` and are removed before cross-host redirects.
Metadata reads are bounded, package downloads use atomic temporary files with
transient-error retries, and checksum or Debian-package validation failures
remove the invalid output before the build stops.

`dev-mcp-entrypoint.sh` emits a runtime visibility probe on container start
(ADR-0543):

- `WARN: SYCL GPU NOT detected`
- `WARN: HIP HSA GPU agent NOT detected`

Either warning means the host kernel has revved past the pinned userspace ABI.
Bump the ARG and rebuild rather than working around the fallback. SYCL
detection accepts anchored Level Zero and OpenCL GPU records; HIP detection
accepts anchored `Name: gfx...` or `Device Type: GPU` records, never
diagnostic prose that merely mentions a GPU token.

Where to find current versions:

- Latest NEO release tag:
  `https://github.com/intel/compute-runtime/releases/latest`
- ROCm release notes:
  `https://rocm.docs.amd.com/en/latest/about/release-notes.html`
- ROCm images: `https://hub.docker.com/r/rocm/dev-ubuntu-26.04/tags`

Do not consult `https://repo.radeon.com/rocm/apt/` for the current version;
that channel has been frozen at 7.2.4 since AMD moved to TheRock.

### Vulkan loader and lavapipe

The image carries the Vulkan loader (`libvulkan1`), its headers
(`libvulkan-dev`), Mesa's Vulkan drivers (`mesa-vulkan-drivers`, which include
lavapipe, the CPU Vulkan driver) and `vulkaninfo` (`vulkan-tools`), all from
the Ubuntu 26.04 archive of the digest-pinned `DEV_BASE`
([ADR-3137](../adr/3137-dev-image-vulkan-lavapipe.md)). This is not a Vulkan
backend (that was removed by ADR-0726): it lets meson find
`dependency('vulkan')` in every lane that builds in this image, so the VMAFx
Vulkan frame-import tests build there and the hosted clang-tidy lanes measure
them, and it gives every lane a Vulkan device without a GPU.

Check it in a running container; no GPU has to be passed in:

```bash
docker exec vmaf-dev-mcp vulkaninfo --summary
```

The device list includes `llvmpipe` (driver `llvmpipe`, type
`PHYSICAL_DEVICE_TYPE_CPU`). The image build runs the same check and fails
when the loader does not enumerate lavapipe. On a host that passes a GPU in,
the GPU's Vulkan device is listed beside it; tests that need a GPU's Vulkan
memory pick the GPU and ignore lavapipe. Real-GPU Vulkan runs remain a
separate signal from the CPU runs.

## History

- **2026-09-06, stale checkout.** The container was rebuilt specifically to
  pick up the GPU default-model fixes (#1307, #1312, #1324). The checkout was
  28 commits behind, and the resulting image had none of them. It was noticed
  only because a test file was missing; a GPU smoke run instead would have
  reported green numbers for code that was not in the image. This is why
  `scripts/dev/check-container-source.sh` and the `source_rev` marker exist
  (ADR-1195).
- **`HSA_OVERRIDE_GFX_VERSION` removal.** The variable used to be `10.3.0`.
  AMD `gfx1036` (Raphael iGPU, RDNA2 IP rev 10.3.6) was not on the ROCm
  6.x/7.x supported-GPU allowlist, so without the override `hsa_init()`
  returned `HSA_STATUS_ERROR_OUT_OF_RESOURCES` and `rocminfo` reported "Unable
  to open /dev/kfd read-write: Invalid argument" even with `/dev/kfd`
  bind-mounted. ROCm 10 supports `gfx1036` natively, so ADR-1225 superseded
  ADR-0542 on this point.
