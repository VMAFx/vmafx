# Base: NVIDIA CUDA ≥13.2 devel on Ubuntu 24.04. Non-conservative pin per ADR D27 —
# we follow latest-stable CUDA aggressively because the fork's value is GPU perf on
# current hardware; being one release behind costs ~10-30% on kernel-bound stages.
# Digest-pinned for reproducibility of *this* snapshot; bump both tag + digest on
# every stable CUDA release. Gives us nvcc + cudart-dev without Ubuntu's stale
# 'nvidia-cuda-toolkit' apt package (24.04 still ships CUDA 12.x).
FROM nvidia/cuda:13.2.1-devel-ubuntu24.04@sha256:44a9504c6dfb50b1241464241b02a93871928f373de6f5a644cf5fe9f080aa63

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

ENV DEBIAN_FRONTEND=noninteractive

# pipefail for RUNs that use `|` (DL4006).
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# ---------- system dependencies ----------
# CUDA toolkit + cudart are already present in the base image.
# DL3008: we install tracked security-updated versions from the Ubuntu
# 24.04 archive; pinning every patch version would break on every
# upstream security update. apt-get update + install happens in one
# layer so the cache stays consistent (DL3009).
# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
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
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# ---------- Intel oneAPI (SYCL, optional) ----------
# Modern keyring pattern (apt-key was deprecated in Ubuntu 22.04+).
# hadolint ignore=DL3008
RUN if [ "$ENABLE_SYCL" = "true" ]; then \
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
            level-zero-dev && \
        rm -rf /var/lib/apt/lists/*; \
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
ENV PATH=/vmaf:/vmaf/libvmaf/build/tools:$PATH

# when disabling NVCC, CUDA kernels are JIT-compiled at runtime
RUN make clean && make ENABLE_NVCC=true && make install

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
# CUDA 13.2 libnpp. The npp_*_filter set (scale_npp, transpose_npp, etc.)
# is unrelated to VMAF; cuvid + nvdec + nvenc + libvmaf-cuda are what we
# actually use here. Revisit once we move to an FFmpeg release that
# supports CUDA 13 libnpp upstream.
RUN ./configure \
        --enable-nonfree \
        --enable-nvdec \
        --enable-nvenc \
        --enable-cuvid \
        --enable-cuda \
        --enable-cuda-nvcc \
        --enable-libvmaf \
        --enable-ffnvcodec \
        --disable-stripping \
        --nvccflags="${FFMPEG_NVCC_FLAGS}" && \
    make -j"$(nproc)" && \
    make install

# ---------- python tools ----------
WORKDIR /vmaf
RUN pip3 install --no-cache-dir --break-system-packages -r /vmaf/python/requirements.txt
ENV PYTHONPATH=python

RUN mkdir -p /data
ENTRYPOINT ["ffmpeg"]
