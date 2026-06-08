# syntax=docker/dockerfile:1.7
# Base: NVIDIA CUDA ≥13.3 devel on Ubuntu 24.04. Non-conservative pin per ADR D27 —
# we follow latest-stable CUDA aggressively because the fork's value is GPU perf on
# current hardware; being one release behind costs ~10-30% on kernel-bound stages.
# Digest-pinned (sha256) for supply-chain reproducibility; update when upgrading the
# CUDA tag. Gives us nvcc + cudart-dev without Ubuntu's stale 'nvidia-cuda-toolkit'
# apt package (24.04 still ships CUDA 12.x).
FROM nvidia/cuda:13.3.0-devel-ubuntu24.04@sha256:ef2203909e80b8b976cfc672f7e2ae2b00bc0e25c404ee86d89e10a3802f1c52

ARG NV_CODEC_TAG="876af32a202d0de83bd1d36fe74ee0f7fcf86b0d"
ARG FFMPEG_TAG=n8.1
# Broadened gencode: Turing baseline (sm_75) + Ampere (sm_80) + Hopper (sm_90) +
# Blackwell consumer (sm_120). CUDA 13 dropped sm_50/60/70.
# Experimental nvcc feature flags (ADR D27): relaxed-constexpr lets us reuse host
# constexpr in device code; extended-lambda allows __device__ lambdas with capture;
# expt-relaxed-constexpr is required by several Thrust/CUB templates we pull in.
ARG NVCC_FLAGS="-gencode arch=compute_75,code=sm_75 -gencode arch=compute_80,code=sm_80 -gencode arch=compute_90,code=sm_90 -gencode arch=compute_120,code=sm_120 -gencode arch=compute_120,code=compute_120 --expt-relaxed-constexpr --extended-lambda --expt-extended-lambda -O2"
# FFmpeg's configure runs check_nvcc in `-ptx` device-only mode. Two
# constraints from that mode that the libvmaf NVCC_FLAGS above violate:
#   1. `-ptx` only accepts a SINGLE `-gencode` (nvcc fatal: "Option
#      '--ptx (-ptx)' is not allowed when compiling for multiple GPU
#      architectures"). FFmpeg's CUDA filters compile to one PTX target
#      and rely on driver JIT for newer GPUs, so one arch is sufficient.
#   2. The experimental host+device flags above (`--extended-lambda` et
#      al.) are device-only-incompatible.
# compute_75 (Turing) is FFmpeg's own fallback default for modern nvcc;
# the PTX is forward-compatible with everything newer via JIT.
ARG FFMPEG_NVCC_FLAGS="-gencode arch=compute_75,code=sm_75 -O2"
ARG ENABLE_SYCL=false

ARG DEBIAN_FRONTEND=noninteractive

# pipefail for RUNs that use `|` (DL4006).
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# ---------- system dependencies ----------
# CUDA toolkit + cudart are already present in the base image.
# DL3008: we install tracked security-updated versions from the Ubuntu
# 24.04 archive; pinning every patch version would break on every
# upstream security update. apt-get update + install happens in one
# layer so the cache stays consistent (DL3009).
# BuildKit cache mounts (`type=cache,target=/var/cache/apt` + `/var/lib/apt`)
# preserve the apt package + index caches across builds so subsequent rebuilds
# skip the network fetch. `sharing=locked` serialises concurrent BuildKit jobs
# against the same cache. We INTENTIONALLY drop `rm -rf /var/lib/apt/lists/*`
# here — with the cache mount the lists never make it into the image layer.
# hadolint ignore=DL3008,DL3009
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ccache \
    ninja-build \
    nasm \
    doxygen \
    python3 \
    python3-pip \
    python3-venv \
    xxd \
    clang \
    wget \
    unzip \
    git \
    pkg-config

# ---------- Intel oneAPI (SYCL, optional) ----------
# Modern keyring pattern (apt-key was deprecated in Ubuntu 22.04+).
# hadolint ignore=DL3008,DL3009
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    if [ "$ENABLE_SYCL" = "true" ]; then \
        apt-get update && apt-get install -y --no-install-recommends gnupg2 ca-certificates && \
        wget -qO- https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB \
            | gpg --dearmor -o /usr/share/keyrings/oneapi-archive-keyring.gpg && \
        echo "deb [signed-by=/usr/share/keyrings/oneapi-archive-keyring.gpg] https://apt.repos.intel.com/oneapi all main" \
            > /etc/apt/sources.list.d/oneapi.list && \
        apt-get update && apt-get install -y --no-install-recommends \
            intel-oneapi-compiler-dpcpp-cpp-2025.3 \
            intel-oneapi-runtime-libs-2025.3 \
            libva-dev \
            libva-drm2 \
            level-zero-dev ; \
    fi

# ---------- nv-codec-headers ----------
WORKDIR /tmp/nv-codec
RUN wget -q "https://github.com/FFmpeg/nv-codec-headers/archive/${NV_CODEC_TAG}.zip" && \
    unzip -q "${NV_CODEC_TAG}.zip"
WORKDIR /tmp/nv-codec/nv-codec-headers-${NV_CODEC_TAG}
RUN make && make install
WORKDIR /
RUN rm -rf /tmp/nv-codec

# ---------- build libvmaf ----------
COPY . /vmaf
WORKDIR /vmaf
ENV PATH=/vmaf:/vmaf/core/build/tools:$PATH

# when disabling NVCC, CUDA kernels are JIT-compiled at runtime.
# `ccache` was installed in the build-deps layer; mounting a BuildKit cache at
# /root/.cache/ccache lets sequential rebuilds skip recompile of unchanged
# translation units. Meson auto-detects ccache when it's on PATH.
RUN --mount=type=cache,target=/root/.cache/ccache,sharing=locked \
    CCACHE_DIR=/root/.cache/ccache \
    make clean && make ENABLE_NVCC=true && make install
# Register /usr/local/lib/x86_64-linux-gnu in the dynamic linker cache so the
# installed vmaf binary can find libvmaf.so.3 at runtime. The NVIDIA CUDA
# Ubuntu 24.04 base image does not include the arch-specific subdir in its
# default ld.so.conf; meson strips RPATH on install, so without ldconfig the
# binary exits with a dynamic linker error (vmaf smoke test sees zero stdout).
RUN ldconfig

# ---------- build FFmpeg ----------
RUN wget -q "https://github.com/FFmpeg/FFmpeg/archive/${FFMPEG_TAG}.zip" && \
    unzip -q "${FFMPEG_TAG}.zip" && rm "${FFMPEG_TAG}.zip"

COPY ffmpeg-patches /tmp/ffmpeg-patches
WORKDIR /vmaf/FFmpeg-${FFMPEG_TAG}
# Apply the patch series in series.txt order. Patch 0003 depends on
# fields added by 0001, so applying out of order breaks the build.
RUN set -e; \
    while IFS= read -r line; do \
        case "$line" in ''|\#*) continue ;; esac; \
        echo "Applying ffmpeg-patches/$line"; \
        git apply "/tmp/ffmpeg-patches/$line" 2>/dev/null \
            || patch -p1 < "/tmp/ffmpeg-patches/$line"; \
    done < /tmp/ffmpeg-patches/series.txt

# libvmaf-sycl is auto-detected by FFmpeg's check_pkg_config (added by
# patch 0003); there is no `--enable-libvmaf-sycl` flag to pass. SYCL
# support follows libvmaf's pkg-config (set by `-Denable_sycl=true` at
# libvmaf build time).
# `--enable-libnpp` is omitted: FFmpeg n8.1's libnpp probe carries an
# explicit `die "ERROR: libnpp support is deprecated, version 13.0 and up
# are not supported"` (configure:7335-7336) that fires on the base image's
# CUDA 13.x libnpp. The npp_*_filter set (scale_npp, transpose_npp, etc.)
# is unrelated to VMAF; cuvid + nvdec + nvenc + libvmaf-cuda are what we
# actually use here. Revisit once we move to an FFmpeg release that
# supports CUDA 13 libnpp upstream.
RUN --mount=type=cache,target=/root/.cache/ccache,sharing=locked \
    CCACHE_DIR=/root/.cache/ccache \
    ./configure \
        --enable-nonfree \
        --enable-nvdec \
        --enable-nvenc \
        --enable-cuvid \
        --enable-cuda \
        --enable-cuda-nvcc \
        --enable-libvmaf \
        --enable-ffnvcodec \
        --disable-stripping \
        --cc='ccache gcc' \
        --cxx='ccache g++' \
        --nvccflags="${FFMPEG_NVCC_FLAGS}" && \
    make -j"$(nproc)" && \
    make install

# ---------- python tools ----------
WORKDIR /vmaf
RUN pip3 install --no-cache-dir --break-system-packages -r /vmaf/python/requirements.txt
ENV PYTHONPATH=python

RUN mkdir -p /data
ENTRYPOINT ["ffmpeg"]
