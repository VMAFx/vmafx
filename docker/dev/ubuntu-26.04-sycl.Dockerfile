# syntax=docker/dockerfile:1.26@sha256:ecfaec9ed6d810b56388c508f4121597bfbba70d41a6dfeee4d8cad5f295fc32
# NOTE: intel/oneapi-basekit does not yet publish an ubuntu26.04 variant.
# Pinned to ubuntu24.04 until Intel ships one; track https://hub.docker.com/r/intel/oneapi-basekit/tags.
FROM intel/oneapi-basekit:2025.3.2-0-devel-ubuntu24.04@sha256:79a5e333ff6f773793d124b78047001c51cbcd53035e5100313abf2f771af95a

ENV DEBIAN_FRONTEND=noninteractive \
    INSTALL_LINTERS=1 \
    ENABLE_SYCL=1 \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8

COPY scripts/setup/ubuntu.sh /tmp/ubuntu.sh
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git sudo \
    && bash /tmp/ubuntu.sh \
    && rm -rf /var/lib/apt/lists/* /tmp/ubuntu.sh

SHELL ["/bin/bash", "-c"]
WORKDIR /src
CMD ["bash", "-lc", "source /opt/intel/oneapi/setvars.sh && exec bash"]
