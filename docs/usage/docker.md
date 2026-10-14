# VMAF usage through Docker

Pull a published image to score videos without building anything, or build
FFmpeg-with-libvmaf images from the repository's Dockerfiles. This page lists
the images, shows how to run `vmaf` and `ffmpeg` in them, and covers GPU
access for each variant.

## Published images

Release images are published to `ghcr.io/vmafx/vmafx`. The entry point of every
variant below is the `vmaf` CLI, so arguments after the image name go straight
to `vmaf` ([cli.md](cli.md)).

| Image tag | Backends | GPU access flags |
| --- | --- | --- |
| `<tag>` (and `latest`) | CPU only; amd64 and arm64 | none |
| `<tag>-cuda13` | CUDA 13; amd64 | `--gpus all` |
| `<tag>-rocm10` | HIP on ROCm 10; amd64 | `--device /dev/kfd --device /dev/dri` |
| `<tag>-oneapi2026` | SYCL on oneAPI 2026; amd64 | `--device /dev/dri` |
| `<tag>-server` | MCP server over HTTP (`vmaf-mcp --transport http`) | none |

The `<tag>-oneapi2025` tag is an alias of the oneAPI image, kept for the
pre-2026 name; the `rc.1` and `rc.2` releases publish only the `-oneapi2025`
name. The tester image is documented in [tester-image.md](tester-image.md).
Publishing rules and rebuild triggers are in
[../development/publishing.md](../development/publishing.md).

### What can pull the images

Every published image stores its layers as zstd under OCI media types, at
BuildKit's strongest level ([ADR-1594](../adr/1594-zstd-images-zopfli-zips.md)):
the release images above, the tester images, `vmafx-operator`, `vmafx-server`,
`vmafx-node` and the dev container. Pulling one needs:

| Runtime | Minimum | Released |
| --- | --- | --- |
| Docker Engine | 23.0 | February 2023 |
| Docker Desktop | 4.19 (the first with Engine 23.0) | April 2023 |
| containerd, and Kubernetes through it | 1.5 (Kubernetes 1.26 and later already need 1.6) | May 2021 |
| Podman, CRI-O, skopeo | any current release (zstd since August 2019) | |

Check your Docker with `docker version --format '{{.Server.Version}}'`. An older
one downloads the layers and then stops with:

```text
failed to register layer: ApplyLayer exit status 1 stdout:  stderr: archive/tar: invalid tar header
```

(`docker run` prints the same line after `docker:`.) Debian 12's own `docker.io`
package is 20.10.24 and fails this way: install Docker Engine from Docker's
repository, use Debian 13 or Ubuntu's updated `docker.io`, or pull with Podman.
Images published before ADR-1594 (`v1.0.0-rc.2` and earlier) have gzip layers
and pull with any Docker.

Run a score on the CPU image:

```bash
docker run --rm -v $(pwd):/files ghcr.io/vmafx/vmafx:<tag> \
    --reference /files/ref.y4m --distorted /files/dist.y4m \
    --json --output /files/scores.json
```

Run it on a GPU image, selecting the backend explicitly:

```bash
docker run --rm --gpus all -v $(pwd):/files ghcr.io/vmafx/vmafx:<tag>-cuda13 \
    --backend cuda --reference /files/ref.y4m --distorted /files/dist.y4m

docker run --rm --device /dev/kfd --device /dev/dri -v $(pwd):/files \
    ghcr.io/vmafx/vmafx:<tag>-rocm10 \
    --backend hip --hip_device 0 --reference /files/ref.y4m --distorted /files/dist.y4m

docker run --rm --device /dev/dri -v $(pwd):/files ghcr.io/vmafx/vmafx:<tag>-oneapi2026 \
    --backend sycl --reference /files/ref.y4m --distorted /files/dist.y4m
```

An explicit `--backend` that the image does not provide fails with exit code
100 instead of falling back to the CPU
([cli.md](cli.md#backend-not-available)).

!!! note "Published artifacts come from the container"
    Release binaries and published images are produced inside the container,
    never on a host
    ([ADR-1102](../adr/1102-phase4b9-container-only-publishing.md)).

## Dockerfiles in the repository

| File | Produces | Entry point |
| --- | --- | --- |
| [`Dockerfile`](../../Dockerfile) | `libvmaf` plus FFmpeg with the fork's patch series; CUDA on by default | `ffmpeg` |
| [`Dockerfile.ffmpeg`](../../Dockerfile.ffmpeg) | FFmpeg with NVIDIA nv-codec headers, so hardware decoders feed `libvmaf_cuda` on the GPU; built on the local `vmaf` image, selected by digest | `ffmpeg` |
| [`docker/Dockerfile.production`](../../docker/Dockerfile.production) | The published CPU image (`--target cli`) and the MCP server image (`--target server`) | `vmaf` / `vmaf-mcp` |
| [`docker/Dockerfile.production-gpu`](../../docker/Dockerfile.production-gpu) | The published CUDA, ROCm and oneAPI images | `vmaf` |
| [`Dockerfile.go-server`](../../Dockerfile.go-server), `docker/Dockerfile.controller`, `.node`, `.operator` | The Go services `vmafx-server`, `vmafx-controller`, `vmafx-node` and `vmafx-operator` | the service binary |
| `docker/Dockerfile.tester` | The hardware-tester image ([tester-image.md](tester-image.md)) | tester tool |
| [`dev/Containerfile`](../../dev/Containerfile) | The `vmaf-dev-mcp` developer container | shell |

The developer container bakes in every backend toolchain; the operator guide is
[../development/dev-mcp.md](../development/dev-mcp.md).

## Build the FFmpeg image

Install Docker, then from the repository root:

```bash
docker build -t vmaf .
```

The resulting image's entry point is `ffmpeg`, so arguments are forwarded
directly:

```bash
docker run --rm -v $(pwd):/files vmaf \
    -i /files/distorted.y4m \
    -i /files/reference.y4m \
    -lavfi libvmaf \
    -f null -
```

To run the `vmaf` CLI from this image instead, override the entry point:

```bash
docker run --rm -v $(pwd):/files --entrypoint vmaf vmaf \
    --reference /files/reference.y4m --distorted /files/distorted.y4m
```

To verify the build, list the filter options (`ffmpeg` is the entry point):

```bash
docker run --rm vmaf -h filter=libvmaf
```

### Build arguments

| Argument | Default | Effect |
| --- | --- | --- |
| `ENABLE_SYCL` | `false` | `true` bundles Intel oneAPI for the SYCL backend. |
| `FFMPEG_TAG` | `n9.0.2` | FFmpeg release the patch series is applied to. |
| `FFMPEG_REMOTE` | FFmpeg's GitHub repository | Where FFmpeg is fetched from. |
| `NV_CODEC_TAG` | `n13.1.15.0` | nv-codec-headers release. |
| `NVCC_FLAGS` | Turing, Ampere, Hopper and Blackwell-consumer `-gencode` set | CUDA architectures libvmaf is compiled for. |
| `FFMPEG_NVCC_FLAGS` | one `compute_75` target | The single PTX target FFmpeg's CUDA filters use. |

## Run with CUDA

1. Install the
   [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html)
   to give containers GPU access.
2. Run the `libvmaf_cuda` filter. The default image is built with CUDA, so the
   filter is available out of the box:

    ```bash
    docker run --gpus all --rm -v $(pwd):/files vmaf \
        -i /files/distorted.y4m \
        -i /files/reference.y4m \
        -lavfi "[0:v][1:v]libvmaf_cuda" \
        -f null -
    ```

3. Optionally, build the dedicated FFmpeg image so that decoding also happens
   on the GPU. CUDA keeps the metric fast, but the `vmaf` CLI is usually
   I/O-bound for compressed inputs. `Dockerfile.ffmpeg` builds on the local
   `vmaf` image and pins it by digest so the base image is exact and satisfies
   OpenSSF Scorecard Pinned-Dependencies requirements. The `docker build -t
   vmaf .` command above must have run on the same machine first. Pass the
   base image digest via `--build-arg VMAF_IMAGE_DIGEST`; building without this
   argument fails because the base digest is required:

    ```bash
    docker build -f Dockerfile.ffmpeg \
      --build-arg VMAF_IMAGE_DIGEST=$(docker image inspect --format '{{.Id}}' vmaf | cut -d: -f2) \
      -t ffmpeg_vmaf .
    ```

Example on two HEVC bitstreams:

```bash
wget https://ultravideo.fi/video/Beauty_3840x2160_120fps_420_8bit_HEVC_RAW.hevc

docker run --gpus all -e NVIDIA_DRIVER_CAPABILITIES=compute,video \
    -v $(pwd):/files ffmpeg_vmaf \
    -y -hwaccel cuda -hwaccel_output_format cuda \
    -i /files/Beauty_3840x2160_120fps_420_8bit_HEVC_RAW.hevc \
    -fps_mode vfr -c:a copy -c:v hevc_nvenc -b:v 2M /files/dist.mp4

docker run --gpus all -e NVIDIA_DRIVER_CAPABILITIES=compute,video \
    -v $(pwd):/files ffmpeg_vmaf \
    -hwaccel cuda -hwaccel_output_format cuda \
    -i /files/Beauty_3840x2160_120fps_420_8bit_HEVC_RAW.hevc \
    -hwaccel cuda -hwaccel_output_format cuda -i /files/dist.mp4 \
    -filter_complex "[0:v]scale_cuda=format=yuv420p[ref];[1:v]scale_cuda=format=yuv420p[dist];[dist][ref]libvmaf_cuda" \
    -f null -
```

!!! note "Pixel formats on the GPU path"
    For 4:2:0 video, convert NV12 to YUV420P with `scale_cuda`, as shown. For
    4:4:4 and 4:2:2 inputs the decoder output can be fed directly, for example
    `-filter_complex "[0:v][1:v]libvmaf_cuda"`.

## Run with SYCL

Build with the SYCL build argument to bundle Intel oneAPI into the image:

```bash
docker build --build-arg ENABLE_SYCL=true -t vmaf-sycl .
```

When the image is built with SYCL, the backend is selected automatically inside
`vmaf`. Use `--no_sycl` to opt out or `--sycl_device N` to pin a device index;
there is no `--sycl` selector flag. See
[backends/sycl/bundling.md](../backends/sycl/bundling.md) for runtime-bundling
notes and [cli.md](cli.md#backend-selection) for the flag grammar.
