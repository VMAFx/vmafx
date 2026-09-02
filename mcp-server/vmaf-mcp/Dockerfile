# syntax=docker/dockerfile:1.27@sha256:bde3983e9c939224420ddaf6b784cc30e09b035a4dea01f581230c50809f372e
#
# Sandboxed vmaf-mcp server — MCP over stdio + pre-built libvmaf binary.
#
# Build from the repo root so the build context has libvmaf and mcp-server/:
#   docker build -f mcp-server/vmaf-mcp/Dockerfile -t vmaf-mcp .
#
# Run (mount your YUV corpus read-only, keep stdio open for MCP):
#   docker run --rm -i \
#       -v /path/to/yuv-corpus:/data:ro \
#       -e VMAF_MCP_ALLOW=/data \
#       vmaf-mcp

FROM ubuntu:26.04@sha256:889d056d5c6c0bfb55789ff3710681d68e50713cb562d2196dc07110599c7a6f AS build

ENV DEBIAN_FRONTEND=noninteractive
# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential meson ninja-build pkg-config nasm \
        python3 python3-pip python3-venv \
        ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src
WORKDIR /src/libvmaf
RUN meson setup ../build -Denable_cuda=false -Denable_sycl=false --buildtype=release \
    && ninja -C ../build

FROM ubuntu:26.04@sha256:889d056d5c6c0bfb55789ff3710681d68e50713cb562d2196dc07110599c7a6f

ENV DEBIAN_FRONTEND=noninteractive \
    VMAF_BIN=/opt/vmaf/vmaf \
    PATH=/opt/vmaf:$PATH

# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip python3-venv \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/tools/vmaf /opt/vmaf/vmaf
COPY --from=build /src/mcp-server/vmaf-mcp /opt/vmaf-mcp
COPY --from=build /src/model /opt/vmaf/model
COPY --from=build /src/testdata /opt/vmaf/testdata

RUN pip install --no-cache-dir --break-system-packages /opt/vmaf-mcp

WORKDIR /opt/vmaf
ENTRYPOINT ["vmaf-mcp"]
