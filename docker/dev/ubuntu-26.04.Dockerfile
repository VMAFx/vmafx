# syntax=docker/dockerfile:1.26@sha256:ecfaec9ed6d810b56388c508f4121597bfbba70d41a6dfeee4d8cad5f295fc32
FROM ubuntu:26.04@sha256:53958ec7b67c2c9355df922dd08dbf0360611f8c3cdb656875e81873db9ffdba

ENV DEBIAN_FRONTEND=noninteractive \
    INSTALL_LINTERS=1 \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8

COPY scripts/setup/ubuntu.sh /tmp/ubuntu.sh
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git sudo \
    && bash /tmp/ubuntu.sh \
    && rm -rf /var/lib/apt/lists/* /tmp/ubuntu.sh

WORKDIR /src
CMD ["bash"]
