# Alpine 3.20 — minimal CPU-only image. musl libc so GPU backends are unavailable.
# Useful for CI runners, sanity-checking against upstream, and tiny container distros.
FROM alpine:3.23@sha256:5b10f432ef3da1b8d4c7eb6c487f2f5a8f096bc91145e68878dd4a5019afde11

RUN apk add --no-cache \
      build-base clang clang-extra-tools cppcheck \
      meson ninja nasm pkgconf \
      python3 py3-pip \
      git curl wget \
      doxygen shellcheck shfmt \
      ffmpeg-dev

COPY . /vmaf
WORKDIR /vmaf

RUN meson setup build core \
      -Denable_cuda=false -Denable_sycl=false \
      --buildtype=release \
 && ninja -C build

ENV PATH="/vmaf/build/tools:${PATH}"
ENTRYPOINT ["vmaf"]
