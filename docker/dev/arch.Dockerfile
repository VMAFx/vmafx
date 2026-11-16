# Arch Linux dev image. CPU-only by default; toggle ENABLE_CUDA / ENABLE_SYCL.
# Arch rolls forward aggressively — use this for the "latest toolchain" build.
FROM archlinux:latest@sha256:d4594bb2300cba11f41c2f27923eb56f63d9988909c402f9c95517d1e362c899

ARG ENABLE_CUDA=false
ARG ENABLE_SYCL=false

RUN pacman -Syu --noconfirm --needed \
      base-devel clang cppcheck \
      meson ninja nasm pkgconf \
      python python-pip \
      git curl wget \
      doxygen shellcheck shfmt \
      ffmpeg zimg \
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
