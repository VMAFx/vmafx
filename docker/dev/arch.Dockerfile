# Arch Linux dev image. CPU-only by default; toggle ENABLE_CUDA / ENABLE_SYCL.
# Arch rolls forward aggressively — use this for the "latest toolchain" build.
FROM archlinux:latest@sha256:ff410a88e200b133e577f5730b7bfa324e26a333075ee056bf45e911c6ac5671

ARG ENABLE_CUDA=false
ARG ENABLE_SYCL=false

RUN pacman -Syu --noconfirm --needed \
      base-devel clang cppcheck \
      meson ninja nasm pkgconf \
      python python-pip \
      git curl wget \
      doxygen shellcheck shfmt \
      ffmpeg \
 && pacman -Scc --noconfirm

RUN if [ "$ENABLE_CUDA" = "true" ]; then pacman -S --noconfirm --needed cuda cuda-tools; fi

# oneAPI on Arch is via AUR or direct install — leave to user rather than ship AUR helper.
RUN if [ "$ENABLE_SYCL" = "true" ]; then \
      echo "oneAPI on Arch images: bind-mount /opt/intel/oneapi from host, or bake via AUR helper."; \
    fi

COPY . /vmaf
WORKDIR /vmaf

RUN meson setup build core \
      -Denable_cuda=$ENABLE_CUDA -Denable_sycl=$ENABLE_SYCL \
      --buildtype=release \
 && ninja -C build

ENV PATH="/vmaf/build/tools:${PATH}"
ENTRYPOINT ["vmaf"]
