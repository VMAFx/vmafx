# syntax=docker/dockerfile:1.26@sha256:ecfaec9ed6d810b56388c508f4121597bfbba70d41a6dfeee4d8cad5f295fc32
# Non-conservative CUDA pin per ADR D27 — dev Dockerfile tracks the same
# major.minor as the prod Dockerfile (currently 13.3). Bump together.
FROM nvidia/cuda:13.3.0-devel-ubuntu26.04@sha256:243be03aa10331842755b7e5c044aefb0c97978e8065d27d40aed4663094c900

ENV DEBIAN_FRONTEND=noninteractive \
    INSTALL_LINTERS=1 \
    ENABLE_CUDA=1 \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8 \
    PATH=/usr/local/cuda/bin:$PATH \
    # Experimental nvcc feature flags — see ADR D27 rationale. These are
    # stable in the mainline compiler, but gated behind --expt flags because
    # NVIDIA reserves the right to tighten the relaxed rules later.
    NVCCFLAGS="--expt-relaxed-constexpr --extended-lambda --expt-extended-lambda"

COPY scripts/setup/ubuntu.sh /tmp/ubuntu.sh
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git sudo \
    && bash /tmp/ubuntu.sh \
    && rm -rf /var/lib/apt/lists/* /tmp/ubuntu.sh

WORKDIR /src
CMD ["bash"]
