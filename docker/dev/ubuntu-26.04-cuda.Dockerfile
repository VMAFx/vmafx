# syntax=docker/dockerfile:1.27@sha256:bde3983e9c939224420ddaf6b784cc30e09b035a4dea01f581230c50809f372e
# Base: digest-pinned Ubuntu 26.04. CUDA compiler + runtime headers are installed
# explicitly via scripts/ci/install-cuda-toolkit.sh from NVIDIA's apt repository (ADR-1306),
# avoiding the OCI image publication lag that blocked bumping CUDA releases.
ARG CUDA_BUILDER="ubuntu:26.04@sha256:065aa8626841f16057b78b027b464cd59c4fac36c204c72dcbf48d66f5fa8a96"
FROM ${CUDA_BUILDER}
ARG VMAFX_REPO_ROOT=/tmp

# CUDA toolkit comes from scripts/ci/install-cuda-toolkit.sh rather than ubuntu.sh's apt path.
ENV DEBIAN_FRONTEND=noninteractive \
    INSTALL_LINTERS=true \
    VMAFX_TOOLS_VENV=/opt/vmafx-tools \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8 \
    CUDA_HOME=/usr/local/cuda \
    PATH=/opt/vmafx-tools/bin:/usr/local/cuda/bin:$PATH \
    LD_LIBRARY_PATH=/usr/local/cuda/lib64 \
    # Experimental nvcc feature flags — see ADR D27 rationale. These are
    # stable in the mainline compiler, but gated behind --expt flags because
    # NVIDIA reserves the right to tighten the relaxed rules later.
    NVCCFLAGS="--expt-relaxed-constexpr --extended-lambda --expt-extended-lambda"

COPY build-config.env scripts/ci/install-cuda-toolkit.sh scripts/setup/install-shfmt.sh scripts/setup/ubuntu.sh /tmp/
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
    && /tmp/install-cuda-toolkit.sh --mode=builder /tmp \
    && bash /tmp/ubuntu.sh \
    && rm -rf /var/lib/apt/lists/* /tmp/ubuntu.sh /tmp/install-shfmt.sh /tmp/build-config.env /tmp/install-cuda-toolkit.sh /tmp/requirements

WORKDIR /src
CMD ["bash"]
