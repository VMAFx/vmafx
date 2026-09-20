# syntax=docker/dockerfile:1.27@sha256:bde3983e9c939224420ddaf6b784cc30e09b035a4dea01f581230c50809f372e
# Non-conservative CUDA pin per ADR D27 — dev Dockerfile tracks the same
# major.minor as the prod Dockerfile (currently 13.3). Bump together.
FROM nvidia/cuda:13.4.1-devel-ubuntu26.04@sha256:fe678162c7114158e170f2f727a7582162a3a55d7090ed31fac04f2dbc173378

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
