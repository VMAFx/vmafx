# VMAF Usage through Docker

Two Dockerfiles ship with the fork:

- [`Dockerfile`](../../Dockerfile) — builds `libvmaf` and wires FFmpeg as the
  container entrypoint. CUDA support is enabled by default; SYCL can be enabled
  via build arg `ENABLE_SYCL=true`.
- [`Dockerfile.ffmpeg`](../../Dockerfile.ffmpeg) — a dedicated image that builds
  FFmpeg with NVIDIA's nv-codec headers, so that hardware decoders can feed
  the `libvmaf_cuda` filter end-to-end on GPU.

Install Docker, then from the VMAF directory run:

```bash
docker build -t vmaf .
```

The resulting image's entrypoint is `ffmpeg`, so arguments are forwarded
directly:

```bash
docker run --rm -v $(pwd):/files vmaf \
    -i /files/reference.y4m \
    -i /files/distorted.y4m \
    -lavfi libvmaf \
    -f null -
```

## Using Docker with CUDA support

To run containers with GPU access install the
[NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html).
The default image is built with CUDA, so `libvmaf_cuda` is available out of
the box:

```bash
docker run --gpus all --rm -v $(pwd):/files vmaf \
    -i /files/reference.y4m \
    -i /files/distorted.y4m \
    -lavfi "[0:v][1:v]libvmaf_cuda" \
    -f null -
```

While CUDA keeps the metric itself fast, the `vmaf` CLI is usually I/O-bound
for compressed inputs. For the best throughput, use `Dockerfile.ffmpeg` so
that decoding also happens on the GPU:

```bash
docker build -f Dockerfile.ffmpeg -t ffmpeg_vmaf .
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
    -filter_complex "[0:v]scale_cuda=format=yuv420p[ref];[1:v]scale_cuda=format=yuv420p[dist];[ref][dist]libvmaf_cuda" \
    -f null -
```

For 4:2:0 video you need to convert NV12 to YUV420P with `scale_cuda` as shown;
for 4:4:4 / 4:2:2 inputs the decoder output can be fed directly, e.g.
`-filter_complex "[0:v][1:v]libvmaf_cuda"`.

## Using Docker with SYCL support

Build with the SYCL build arg to bundle Intel oneAPI into the image:

```bash
docker build --build-arg ENABLE_SYCL=true -t vmaf-sycl .
```

When the image is built with SYCL, the backend is auto-selected
inside `vmaf`. Use `--no_sycl` to opt out, or `--sycl_device N` to
pin a specific device index — there is no `--sycl` selector flag.
See [backends/sycl/bundling.md](../backends/sycl/bundling.md) for
runtime-bundling notes relevant to containerized deployments, and
[cli.md](cli.md#backend-selection) for the full flag grammar.
