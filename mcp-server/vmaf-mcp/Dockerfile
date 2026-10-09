# syntax=docker/dockerfile:1.27@sha256:4edf897a3ffa55b89f906fc8cc78afdb3f1834cc9c7083565e611a8a7d5fe99e
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

FROM ubuntu:26.04@sha256:88a381d5b5eeb2b35d3ad70925a362c37ce569daf43ede89ff818ec20e4d3794 AS build

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
WORKDIR /src
RUN python3 -m pip install --no-cache-dir --break-system-packages --ignore-installed --require-hashes \
        -r requirements/locks/package-build.txt \
    && python3 -m build --no-isolation --wheel --outdir /wheels mcp-server/vmaf-mcp

FROM ubuntu:26.04@sha256:88a381d5b5eeb2b35d3ad70925a362c37ce569daf43ede89ff818ec20e4d3794

ENV DEBIAN_FRONTEND=noninteractive \
    VMAF_BIN=/opt/vmaf/vmaf \
    PATH=/opt/vmaf:$PATH

# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip python3-venv \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/tools/vmaf /opt/vmaf/vmaf
COPY --from=build /wheels/vmaf_mcp-*.whl /tmp/
COPY --from=build /src/mcp-server/vmaf-mcp/requirements-runtime-lock.txt /tmp/
COPY --from=build /src/model /opt/vmaf/model
COPY --from=build /src/testdata /opt/vmaf/testdata

RUN pip install --no-cache-dir --break-system-packages --ignore-installed --require-hashes \
        -r /tmp/requirements-runtime-lock.txt \
    && pip install --no-cache-dir --break-system-packages --ignore-installed --no-deps \
        /tmp/vmaf_mcp-*.whl

WORKDIR /opt/vmaf
ENTRYPOINT ["vmaf-mcp"]
