# Alpine 3.20 — minimal CPU-only image. musl libc so GPU backends are unavailable.
# Useful for CI runners, sanity-checking against upstream, and tiny container distros.
FROM alpine:3.24@sha256:a2d49ea686c2adfe3c992e47dc3b5e7fa6e6b5055609400dc2acaeb241c829f4

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
