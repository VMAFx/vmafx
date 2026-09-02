<!-- markdownlint-disable MD060 -->
# vmafx-node: worker node image

`vmafx-node` is the VMAFX worker binary and its container image. Each node
connects to the controller, receives encoding jobs, runs `ffmpeg` for encoding,
and reports scores back. This document covers the node's ffmpeg setup, build,
codec matrix, and operational considerations.

See [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) (Phase 4b
umbrella) and [ADR-0717](../adr/0717-vmafx-node-ffmpeg-latest.md) (ffmpeg
version policy) for the decision record.

## Quick start

```bash
# Build the CPU-only node image
docker buildx build --target node-cpu \
  -f docker/Dockerfile.node \
  -t vmafx-node:dev .

# Verify ffmpeg version
docker run --rm --entrypoint /usr/local/bin/ffmpeg \
  vmafx-node:dev -version | head -1
# → ffmpeg version n9.0.1 ...

# Verify codec inventory
docker run --rm --entrypoint /usr/local/bin/ffmpeg \
  vmafx-node:dev -encoders \
  | grep -E 'libx264|libx265|libsvtav1|libvpx'
```

## Image variants

| Target | GPU scoring runtime | FFmpeg encoders | Use case |
|---|---|---|---|
| `node-cpu` | none | software only | Development, CI, low-volume workloads |
| `node-cuda` | NVIDIA (CUDA 13.3.1) | software only | GPU-accelerated VMAF scoring on NVIDIA pools |
| `node-rocm` | AMD (ROCm 7.2.4) | software only | GPU-accelerated VMAF scoring on AMD pools |
| `node-sycl` | Intel (oneAPI 2025.3.1) | software only | GPU-accelerated VMAF scoring on Intel Arc / Xe pools |

All four variants carry **the same ffmpeg binary** (built in the shared
`ffmpeg-builder-cpu` stage). The CUDA / ROCm / SYCL variants differ only in
the additional GPU runtime libraries copied into the final stage. That shared
FFmpeg build does not enable NVENC, AMF, or QSV encoders; the GPU runtimes
accelerate VMAF scoring, not video encoding.

## ffmpeg version policy

The node image pins ffmpeg to the **latest stable tagged release**
(`FFMPEG_TAG=n9.0.1` as of 2026-08-31). The tag is a Docker build argument:

```bash
# Override to test against a specific release
docker buildx build --target node-cpu \
  --build-arg FFMPEG_TAG=n9.0.1 \
  -f docker/Dockerfile.node \
  -t vmafx-node:n9.0.1-test .
```

**Update cadence**: `FFMPEG_TAG` is updated in the same PR that bumps
`ffmpeg-patches/README.md` after a patch-series refresh. The dev Containerfile
(`dev/Containerfile`) and the node Dockerfile are updated together so both
environments track the same base.

See [ADR-0717](../adr/0717-vmafx-node-ffmpeg-latest.md) for the rationale
behind pinning to a tag rather than a rolling release branch.

## ffmpeg-patches

The node image applies the fork's full 17-patch series from `ffmpeg-patches/`
during the `ffmpeg-builder-cpu` stage. The patches:

- Carry the fork's libvmaf selector/filter integrations plus `vmaf_pre` and
  `libvmaf_tune`; the node's shared FFmpeg build enables only CPU libvmaf.
- Add the vmaf-tune `qpfile` AVOption to libx264 / libsvtav1.
- Wire the `-pass-autotune` and `-vmaf-profile` CLI glue.

If a patch fails to apply against a new ffmpeg tag, the build fails at the
`git am` step with a message naming the offending patch. Fix the patch before
bumping the tag.

The apply command mirrors the dev container approach:

```bash
git am --3way /src/ffmpeg-patches/<patch>.patch
```

See [ffmpeg-patches/README.md](../../ffmpeg-patches/README.md) for the full
verification gate and invariants.

## Codec matrix

| Codec | Direction | Library | Notes |
|---|---|---|---|
| H.264 | encode | libx264 | ubiquitous SW encoder |
| H.265 / HEVC | encode | libx265 | SW encoder |
| VP9 | encode | libvpx | Google VP9 SW encoder |
| AV1 | encode | libsvtav1 | Intel/Netflix SVT-AV1 (production AV1 lane) |
| AV1 | decode | libdav1d | Fast AV1 decoder |

**libaom is excluded**: `ffmpeg-patches/0007` references `aom_roi_map_t`
fields not present in any released libaom. SVT-AV1 covers the AV1 production
lane. See `dev/Containerfile` §NOTE:libaom for the full rationale.

## Encoder startup probe

The `vmafx-node` binary runs `ffmpeg -encoders` at startup and caches the
result. The inventory is logged at `INFO` level:

```json
{"level":"INFO","msg":"encoder inventory","count":42,"available":["libx264","libx265",...]}
```

Missing expected software codecs are logged at `WARN`:

```json
{"level":"WARN","msg":"expected codec missing from ffmpeg -encoders","codec":"libsvtav1","ffmpeg":"/usr/local/bin/ffmpeg"}
```

The probe is non-fatal — a node with a degraded codec matrix still starts and
accepts jobs; jobs requesting unavailable codecs fail at dispatch time with a
clear error message.

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `VMAFX_FFMPEG_BIN` | `ffmpeg` (PATH) | Path to the ffmpeg binary. The Docker image sets this to `/usr/local/bin/ffmpeg`. |
| `VMAFX_GRPC_LISTEN` | `:50052` | gRPC listen address. |
| `VMAFX_LOG_LEVEL` | `INFO` | Log level: DEBUG, INFO, WARN, ERROR. |
| `VMAFX_BACKEND` | unset (cpu) | GPU backend hint passed to libvmaf scoring. Set automatically in node-cuda/rocm/sycl variants. |
| `VMAFX_MODEL_DIR` | `/usr/local/share/vmafx/model` | Directory of VMAF model JSON/ONNX files. |

## Building locally

```bash
# CPU variant (no GPU SDK required)
docker buildx build --target node-cpu \
  -f docker/Dockerfile.node \
  -t vmafx-node:local .

# CUDA variant (references the pinned CUDA 13.3.1 runtime image)
docker buildx build --target node-cuda \
  -f docker/Dockerfile.node \
  -t vmafx-node:local-cuda .
```

Build time is approximately 10–15 minutes on a standard developer machine
(dominated by the ffmpeg compile). Use `--cache-from` or BuildKit layer cache
to speed up subsequent builds.

## Smoke tests

After building:

```bash
# Confirm the node binary carries the release/build version without starting
# its long-running gRPC service
docker run --rm vmafx-node:local --version

# Confirm ffmpeg version
docker run --rm --entrypoint /usr/local/bin/ffmpeg \
  vmafx-node:local -version | head -1

# Confirm codec inventory includes expected software encoders
docker run --rm --entrypoint /usr/local/bin/ffmpeg \
  vmafx-node:local -hide_banner -encoders 2>/dev/null \
  | grep -E 'libx264|libx265|libsvtav1|libvpx'

# Confirm libvmaf and all node runtime libraries resolve
docker run --rm --entrypoint /usr/local/bin/vmaf \
  vmafx-node:local --version
```

The image build derives FFmpeg's non-glibc shared-library closure from `ldd`
inside the native build stage. This avoids architecture-specific
`/usr/lib/x86_64-linux-gnu` copies: an arm64 build resolves and stages its
`aarch64-linux-gnu` libraries automatically. The release workflow runs both
the node's `--version`, `vmaf --version`, and `ffmpeg -version` in the
published image without a success-masking fallback. Release builds inject the
published tag into `pkg/version.version`; `dev` identifies a non-release build.

The full smoke-test sequence from the task brief (including Netflix golden
scoring) requires the `python/test/resource/yuv/` fixtures to be mounted:

```bash
docker run --rm \
  -v "$PWD/python/test/resource:/data:ro" \
  --entrypoint /usr/local/bin/vmaf \
  vmafx-node:local \
    --reference /data/yuv/src01_hrc00_576x324.yuv \
    --distorted  /data/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 \
    --pixel_format 420 --bitdepth 8
# Expected VMAF score: ~76.668 (Netflix golden)
```

## Relationship to dev container

The dev container (`dev/Containerfile`) also builds ffmpeg (currently n9.0.1).
The two builds are intentionally separate:

- Dev container: full workbench with CUDA toolchain, oneAPI, MCP server, Python
  environment. Not a delivery artifact.
- Node image: lean production runtime based on distroless cc-debian13. Ships
  the `vmafx-node` binary + ffmpeg + libvmaf only.

When the patch series base is bumped, both the dev
container's `FFMPEG_TAG` ARG and the node Dockerfile's `FFMPEG_TAG` ARG are
updated in the same PR.

See [dev-mcp.md](dev-mcp.md) for the dev container operator guide.
