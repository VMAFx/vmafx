# syntax=docker/dockerfile:1.27@sha256:bde3983e9c939224420ddaf6b784cc30e09b035a4dea01f581230c50809f372e
# NOTE: this is the `intel/oneapi` repository, NOT `intel/oneapi-basekit`.
# Intel retired the basekit repo at 2025.3.2 and continued the toolkit image
# under the plain name, which is where the 2026 tags and the ubuntu26.04
# variant live. This file previously carried a comment saying no 26.04 variant
# existed and pinned ubuntu24.04, so a file named ubuntu-26.04-sycl was
# building on 24.04. Track https://hub.docker.com/r/intel/oneapi/tags.
#
# Unlike the release images this stays on Intel's own image on purpose: the
# docker/dev/ matrix exists to prove the build survives distros the release
# track does not use.
FROM intel/oneapi:2026.1.0-devel-ubuntu26.04@sha256:45de6b5bf2083179b420f01bd68781da218d6b72dcba7f1e35aa0c108526f819

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
