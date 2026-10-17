# syntax=docker/dockerfile:1.27@sha256:bde3983e9c939224420ddaf6b784cc30e09b035a4dea01f581230c50809f372e
# Base: digest-pinned Ubuntu 26.04. CUDA compiler + runtime headers are installed
# explicitly via scripts/ci/install-cuda-toolkit.sh from NVIDIA's apt repository (ADR-1306),
# avoiding the OCI image publication lag that blocked bumping CUDA releases.
ARG CUDA_BUILDER="ubuntu:26.04@sha256:c098726dd9b9733f3e885f8de0ef375206a311b0a317eadc40d347b1115fb944"
FROM ${CUDA_BUILDER}

ENV DEBIAN_FRONTEND=noninteractive \
    INSTALL_LINTERS=1 \
    ENABLE_CUDA=1 \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8 \
    CUDA_HOME=/usr/local/cuda \
    PATH=/usr/local/cuda/bin:$PATH \
    LD_LIBRARY_PATH=/usr/local/cuda/lib64 \
    # Experimental nvcc feature flags — see ADR D27 rationale. These are
    # stable in the mainline compiler, but gated behind --expt flags because
    # NVIDIA reserves the right to tighten the relaxed rules later.
    NVCCFLAGS="--expt-relaxed-constexpr --extended-lambda --expt-extended-lambda"

COPY build-config.env scripts/ci/install-cuda-toolkit.sh /tmp/
RUN /tmp/install-cuda-toolkit.sh --mode=builder /tmp \
    && rm -f /tmp/install-cuda-toolkit.sh /tmp/build-config.env

COPY scripts/setup/ubuntu.sh /tmp/ubuntu.sh
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git sudo \
    && bash /tmp/ubuntu.sh \
    && rm -rf /var/lib/apt/lists/* /tmp/ubuntu.sh

WORKDIR /src
CMD ["bash"]
