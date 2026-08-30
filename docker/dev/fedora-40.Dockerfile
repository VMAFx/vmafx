FROM fedora:46@sha256:a9bab18d01cf2c2cf62f3e79c72623405bce14ac062995cc6651a3073c802e41

ARG ENABLE_CUDA=false
ARG ENABLE_SYCL=false

RUN dnf groupinstall -y "Development Tools" \
 && dnf install -y \
      clang clang-tools-extra cppcheck \
      meson ninja-build nasm pkgconf-pkg-config \
      python3 python3-pip \
      git curl wget \
      doxygen shellcheck \
      ffmpeg-devel --allowerasing \
 && dnf clean all

RUN if [ "$ENABLE_CUDA" = "true" ]; then \
      dnf config-manager --add-repo https://developer.download.nvidia.com/compute/cuda/repos/fedora40/x86_64/cuda-fedora40.repo \
   && dnf install -y cuda-toolkit \
   && dnf clean all; \
    fi

RUN if [ "$ENABLE_SYCL" = "true" ]; then \
      tee /etc/yum.repos.d/oneAPI.repo <<'EOF' \
[oneAPI]\n\
name=Intel(R) oneAPI repository\n\
baseurl=https://yum.repos.intel.com/oneapi\n\
enabled=1\n\
gpgcheck=1\n\
repo_gpgcheck=1\n\
gpgkey=https://yum.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB\n\
EOF\n\
   && dnf install -y intel-oneapi-compiler-dpcpp-cpp intel-oneapi-runtime-libs level-zero-devel \
   && dnf clean all; \
    fi

COPY . /vmaf
WORKDIR /vmaf

RUN meson setup build core \
      -Denable_cuda=$ENABLE_CUDA -Denable_sycl=$ENABLE_SYCL \
      --buildtype=release \
 && ninja -C build

ENV PATH="/vmaf/build/tools:${PATH}"
ENTRYPOINT ["vmaf"]
