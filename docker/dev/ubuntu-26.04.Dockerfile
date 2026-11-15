# syntax=docker/dockerfile:1.28@sha256:bb22d9815c728170f72750f4e5b0d672e06176142e1d602c7e66c050100b7e5b
FROM ubuntu:26.04@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c

ENV DEBIAN_FRONTEND=noninteractive \
    INSTALL_LINTERS=1 \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8

COPY build-config.env scripts/setup/ubuntu.sh /tmp/
# UBUNTU_APT_CERTIFICATE_BOOTSTRAP: HTTPS-only snapshots need the live archive's CA bundle.
# DL3008: later installs resolve from APT::Snapshot; only this bootstrap is unpinned (ADR-3541).
# hadolint ignore=DL3008
RUN apt-get --error-on=any -o Acquire::Retries=3 update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && . /tmp/build-config.env \
    && printf 'APT::Snapshot "%s";\n' "$UBUNTU_APT_SNAPSHOT" > /etc/apt/apt.conf.d/50ubuntu-snapshot \
    && grep -Eqx 'APT::Snapshot "[0-9]{8}T[0-9]{6}Z";' /etc/apt/apt.conf.d/50ubuntu-snapshot \
    && rm -rf /var/lib/apt/lists/* \
    && apt-get --error-on=any -o Acquire::Retries=3 update \
    && apt-get install -y --no-install-recommends curl git sudo \
    && bash /tmp/ubuntu.sh \
    && rm -rf /var/lib/apt/lists/* /tmp/ubuntu.sh /tmp/build-config.env

WORKDIR /src
CMD ["bash"]
