# syntax=docker/dockerfile:1.29@sha256:572cebbc2b15e6f84c077742952674889cb63cd5e9b80abdffcbe587b6f19183
# NOTE: intel/oneapi-basekit does not yet publish an ubuntu26.04 variant.
# Pinned to ubuntu24.04 until Intel ships one; track https://hub.docker.com/r/intel/oneapi-basekit/tags.
FROM intel/oneapi-basekit:2025.3.2-0-devel-ubuntu24.04@sha256:79a5e333ff6f773793d124b78047001c51cbcd53035e5100313abf2f771af95a
ARG VMAFX_REPO_ROOT=/tmp

# Intel oneAPI DPC++ toolkit comes from the intel/oneapi-basekit base image rather than ubuntu.sh.
ENV DEBIAN_FRONTEND=noninteractive \
    INSTALL_LINTERS=true \
    VMAFX_TOOLS_VENV=/opt/vmafx-tools \
    PATH=/opt/vmafx-tools/bin:$PATH \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8

COPY build-config.env scripts/setup/ubuntu.sh /tmp/
COPY requirements/locks/build.txt requirements/locks/dev-linters.txt /tmp/requirements/locks/
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
    && rm -rf /var/lib/apt/lists/* /tmp/ubuntu.sh /tmp/build-config.env /tmp/requirements

SHELL ["/bin/bash", "-c"]
WORKDIR /src
CMD ["bash", "-lc", "source /opt/intel/oneapi/setvars.sh && exec bash"]
