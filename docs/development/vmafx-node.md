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
docker run --rm vmafx-node:dev ffmpeg -version | head -1
# → ffmpeg version n8.2 ...

# Verify codec inventory
docker run --rm vmafx-node:dev ffmpeg -encoders \
  | grep -E 'libx264|libx265|libsvtav1|libvpx'
```

## Image variants

| Target | GPU | ffmpeg HW encoder | Use case |
|---|---|---|---|
| `node-cpu` | none | software only | Development, CI, low-volume workloads |
| `node-cuda` | NVIDIA (CUDA 12) | `h264_nvenc`, `hevc_nvenc`, `av1_nvenc` | High-throughput on NVIDIA pools |
| `node-rocm` | AMD (ROCm 6) | `h264_amf`, `hevc_amf`, `av1_amf` | AMD Radeon pools |
| `node-sycl` | Intel (oneAPI 2026) | `h264_qsv`, `hevc_qsv`, `av1_qsv` | Intel Arc / Xe pools |

All four variants carry **the same ffmpeg binary** (built in the shared
`ffmpeg-builder-cpu` stage). The CUDA / ROCm / SYCL variants differ only in
the additional GPU runtime libraries copied into the final stage.

## ffmpeg version policy

The node image pins ffmpeg to the **latest stable tagged release**
(`FFMPEG_TAG=n8.2` as of 2026-05-28). The tag is a Docker build argument:

```bash
# Override to test against a specific release
docker buildx build --target node-cpu \
  --build-arg FFMPEG_TAG=n8.3 \
  -f docker/Dockerfile.node \
  -t vmafx-node:n8.3-test .
```

**Update cadence**: `FFMPEG_TAG` is updated in the same PR that bumps
`ffmpeg-patches/README.md` after a patch-series refresh. The dev Containerfile
(`dev/Containerfile`) and the node Dockerfile are updated together so both
environments track the same base.

See [ADR-0717](../adr/0717-vmafx-node-ffmpeg-latest.md) for the rationale
behind pinning to a tag rather than a rolling release branch.

## ffmpeg-patches

The node image applies the fork's full 15-patch series from `ffmpeg-patches/`
during the `ffmpeg-builder-cpu` stage. The patches:

- Enable `libvmaf`, `libvmaf_sycl`, `libvmaf_vulkan`, `libvmaf_cuda`,
  `libvmaf_hip`, `vmaf_pre`, and `libvmaf_tune` FFmpeg filters.
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
| H.264 NVENC | encode (cuda) | host driver | NVIDIA GPU; requires nvidia-container-toolkit |
| HEVC NVENC | encode (cuda) | host driver | NVIDIA GPU |
| H.264 AMF | encode (rocm) | host driver | AMD GPU; requires amdgpu-pro userspace |
| H.264 QSV | encode (sycl) | host + libvpl | Intel GPU; requires iHD VA-API driver |

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
| `VMAFX_NODE_ADDR` | `:50052` | gRPC listen address. |
| `VMAFX_LOG_LEVEL` | `INFO` | Log level: DEBUG, INFO, WARN, ERROR. |
| `VMAFX_BACKEND` | unset (cpu) | GPU backend hint passed to libvmaf scoring. Set automatically in node-cuda/rocm/sycl variants. |
| `VMAF_MODEL_PATH` | `/usr/local/share/vmafx/model` | Directory of VMAF model JSON/ONNX files. |

## Building locally

```bash
# CPU variant (no GPU SDK required)
docker buildx build --target node-cpu \
  -f docker/Dockerfile.node \
  -t vmafx-node:local .

# CUDA variant (references nvidia/cuda:12.0 base image — no local CUDA SDK needed)
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
# Confirm ffmpeg version
docker run --rm vmafx-node:local ffmpeg -version | head -1

# Confirm codec inventory includes expected software encoders
docker run --rm vmafx-node:local \
  ffmpeg -hide_banner -encoders 2>/dev/null \
  | grep -E 'libx264|libx265|libsvtav1|libvpx'

# Confirm node starts and logs the encoder inventory
docker run --rm vmafx-node:local /usr/local/bin/vmafx-node --help 2>&1 | head -5
```

The full smoke-test sequence from the task brief (including Netflix golden
scoring) requires the `python/test/resource/yuv/` fixtures to be mounted:

```bash
docker run --rm \
  -v "$PWD/python/test/resource:/data:ro" \
  vmafx-node:local \
  /usr/local/bin/vmaf \
    --reference /data/yuv/src01_hrc00_576x324.yuv \
    --distorted  /data/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 \
    --pixel_format 420 --bitdepth 8
# Expected VMAF score: ~76.668 (Netflix golden)
```

## Relationship to dev container

The dev container (`dev/Containerfile`) also builds ffmpeg (currently n8.1.1).
The two builds are intentionally separate:

- Dev container: full workbench with CUDA toolchain, oneAPI, MCP server, Python
  environment. Not a delivery artifact.
- Node image: lean production runtime based on distroless cc-debian12. Ships
  the `vmafx-node` binary + ffmpeg + libvmaf only.

When the patch series base is bumped (e.g., n8.1.1 → n8.2), both the dev
container's `FFMPEG_TAG` ARG and the node Dockerfile's `FFMPEG_TAG` ARG are
updated in the same PR.

See [dev-mcp.md](dev-mcp.md) for the dev container operator guide.
