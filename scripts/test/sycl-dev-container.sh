#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# sycl-dev-container.sh - build libvmaf and ffmpeg from this worktree inside the
# SYCL toolchain image and run tests / commands on the Intel GPU.
#
# Usage:
#   scripts/test/sycl-dev-container.sh libvmaf          # JIT libvmaf build + install
#   scripts/test/sycl-dev-container.sh ffmpeg           # ffmpeg + this tree's patch series
#   scripts/test/sycl-dev-container.sh test [args...]   # meson test in the cache build dir
#   scripts/test/sycl-dev-container.sh exec <cmd...>    # run a command with the built ffmpeg/libvmaf
#
# Environment:
#   CONTAINER_RT       container CLI (default: podman, then docker)
#   SYCL_DEV_IMAGE     toolchain image (default: vmaf-dev-mcp:local, built by
#                      dev/docker-compose.yml; it carries icx/icpx, ocloc, libva and libvpl)
#   SYCL_DEV_CC        C compiler for the libvmaf build (default: icx)
#   SYCL_DEV_JOBS      ninja / make parallelism (default: nproc)
#   SYCL_DEV_TIMEOUT   per-run wall-clock cap in seconds (default: 5400)
#   YUV_DIR            fixture directory mounted read-only at /yuv
#   HOST_REPO          host path of the main checkout when running in a devcontainer
#                      (the runtime is the host's, so -v sources must be host paths)

set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: sycl-dev-container.sh {libvmaf|ffmpeg|test [args...]|exec <cmd...>}
EOF
  exit 2
}

[ "$#" -ge 1 ] || usage
MODE="$1"
shift
case "$MODE" in
  libvmaf | ffmpeg | test | exec) ;;
  *) usage ;;
esac
if [ "$MODE" = exec ] && [ "$#" -lt 1 ]; then
  usage
fi

RT="${CONTAINER_RT:-$(command -v podman || command -v docker || true)}"
if [ -z "$RT" ]; then
  echo "error: no container runtime (podman/docker) on PATH." >&2
  exit 127
fi

IMAGE="${SYCL_DEV_IMAGE:-vmaf-dev-mcp:local}"
JOBS="${SYCL_DEV_JOBS:-$(nproc)}"
if [[ ! "$JOBS" =~ ^[1-9][0-9]{0,2}$ ]]; then
  echo "error: SYCL_DEV_JOBS must be an integer from 1 to 999: $JOBS" >&2
  exit 2
fi
TIMEOUT="${SYCL_DEV_TIMEOUT:-5400}"
WORKTREE="$(git rev-parse --show-toplevel)"
COMMON="$(git rev-parse --path-format=absolute --git-common-dir)"
MAIN_ROOT="$(dirname "$COMMON")"
YUV_DIR="${YUV_DIR:-$MAIN_ROOT/python/test/resource/yuv}"

# Rewrite a path under MAIN_ROOT to the same suffix under HOST_REPO.
host_path() {
  local hrepo="${HOST_REPO:-$MAIN_ROOT}"
  case "$1" in
    "$MAIN_ROOT") printf '%s\n' "$hrepo" ;;
    "$MAIN_ROOT"/*) printf '%s\n' "$hrepo${1#"$MAIN_ROOT"}" ;;
    *) printf '%s\n' "$1" ;;
  esac
}

# Script executed inside the container. Quoted heredoc: no host expansion.
INNER="$(
  cat <<'EOS'
set -euo pipefail
MODE="$1"
shift
C=/work/.cache/sycl-dev
mkdir -p "$C"
PC="$C/prefix/lib/x86_64-linux-gnu/pkgconfig:$C/prefix/lib/pkgconfig"

build_libvmaf() {
  if [ ! -f "$C/build/build.ninja" ]; then
    CC="${SYCL_DEV_CC:-icx}" CXX=icpx CC_LD=lld CXX_LD=lld meson setup "$C/build" core \
      -Denable_cuda=false -Denable_sycl=true -Denable_dnn=disabled \
      --buildtype=release -Db_lto=false -Dsycl_icpx_aot_targets= \
      --prefix="$C/prefix"
  fi
  ninja -C "$C/build" -j "$JOBS"
  ninja -C "$C/build" -j "$JOBS" install
  mkdir -p "$C/prefix/include/libvmaf"
  find core "$C/build" -path '*include/libvmaf/*.h' -exec cp -f {} "$C/prefix/include/libvmaf/" \;
}

conf() {
  sed -n "s/^$1=\"\\{0,1\\}\\([^\"]*\\)\"\\{0,1\\}\$/\\1/p" /work/build-config.env | head -n1
}

build_ffmpeg() {
  local commit p
  commit="$(conf FFMPEG_COMMIT)"
  # The FFmpeg source is cached; every build starts again from FFMPEG_COMMIT.
  if [ ! -d "$C/ffmpeg-src" ]; then
    /work/scripts/ci/checkout-annotated-tag.sh "$(conf FFMPEG_REMOTE)" "$(conf FFMPEG_TAG)" \
      "$C/ffmpeg-src"
  fi
  cd "$C/ffmpeg-src"
  # Drops the caller's GIT_* variables and sets the identity `git am` commits with.
  # shellcheck source=scripts/lib/clean-git-env.sh
  . /work/scripts/lib/clean-git-env.sh
  if [ -d .git/rebase-apply ]; then
    git am --abort
  fi
  git reset -q --hard "$commit"
  git clean -q -fdx
  # The shared FFmpeg fix series comes first, as in every other build (ADR-3143).
  bash /work/scripts/ci/ffmpeg-shared-series.sh apply --method am "$C/ffmpeg-src"
  while read -r p; do
    case "$p" in '' | '#'*) continue ;; esac
    if ! git am --3way "/work/ffmpeg-patches/$p"; then
      echo "PATCH FAILED: $p (left in $C/ffmpeg-src; the next build aborts it)" >&2
      exit 1
    fi
  done </work/ffmpeg-patches/series.txt
  PKG_CONFIG_PATH="$PC${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" ./configure \
    --prefix="$C/ffmpeg-prefix" --enable-gpl --enable-version3 --enable-libvmaf \
    --enable-libdav1d --enable-vaapi --enable-libvpl --enable-libvmaf-sycl \
    --disable-filter=vmaf_pre --disable-doc --disable-debug
  make -j"$JOBS" install
  PATH="$C/ffmpeg-prefix/bin:$PATH" LD_LIBRARY_PATH="$C/prefix/lib/x86_64-linux-gnu:$C/prefix/lib:${LD_LIBRARY_PATH:-}" \
    ffmpeg -hide_banner -filters | grep libvmaf
}

case "$MODE" in
  libvmaf) build_libvmaf ;;
  ffmpeg) build_ffmpeg ;;
  test) python3 /work/scripts/ci/run_meson_test.py -- -C "$C/build" --print-errorlogs "$@" ;;
  exec)
    export PATH="$C/ffmpeg-prefix/bin:$C/prefix/bin:$PATH"
    export LD_LIBRARY_PATH="$C/prefix/lib/x86_64-linux-gnu:$C/prefix/lib:${LD_LIBRARY_PATH:-}"
    "$@"
    ;;
esac
EOS
)"

exec timeout --signal=KILL "$TIMEOUT" "$RT" run --rm \
  --device /dev/dri --security-opt label=disable \
  -e UR_L0_USE_IMMEDIATE_COMMANDLISTS=0 \
  -e SYCL_DEV_CC="${SYCL_DEV_CC:-icx}" \
  -e JOBS="$JOBS" \
  -v "$(host_path "$WORKTREE"):/work" \
  -v "$(host_path "$YUV_DIR"):/yuv:ro" \
  -w /work --entrypoint bash "$IMAGE" -c "$INNER" _ "$MODE" "$@"
