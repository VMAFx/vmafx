#!/usr/bin/env bash
# build-and-run.sh — smoke-test the ffmpeg-patches/ series against a pinned
# upstream FFmpeg release tag from build-config.env.
#
# What it does:
#   1. Fetches FFmpeg into a new, disposable $FFMPEG_SRC.
#   2. Applies every patch listed in ffmpeg-patches/series.txt (comment-
#      stripped) in order.
#   3. Configures with the minimum libraries needed for libvmaf + vmaf_pre,
#      builds ffmpeg, and verifies:
#        - `ffmpeg -h filter=libvmaf` lists `tiny_model`
#        - `ffmpeg -h filter=vmaf_pre` exits 0
#   4. Tears down the build tree unless $KEEP_BUILD is set.
#
# Requires libvmaf already installed (`pip`-level: "pkg-config --cflags libvmaf"
# must resolve). Set VMAF_PREFIX to point at a non-standard install prefix.
#
# Applies the complete ordered series with `git am --3way`.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHES_DIR="$(cd "$HERE/.." && pwd)"

# Shared reviewed release tag; an override must also be a stable release.
# shellcheck source=../../build-config.env
# shellcheck disable=SC1091
source "${PATCHES_DIR}/../build-config.env"
: "${FFMPEG_SHA:=${FFMPEG_TAG}}"
if [[ ! "$FFMPEG_SHA" =~ ^n[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
  echo "FFMPEG_SHA must be a stable released tag: $FFMPEG_SHA" >&2
  exit 1
fi
: "${KEEP_BUILD:=}"
: "${VMAF_PREFIX:=}"

if [[ -n "$VMAF_PREFIX" ]]; then
  export PKG_CONFIG_PATH="$VMAF_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  export LD_LIBRARY_PATH="$VMAF_PREFIX/lib:${LD_LIBRARY_PATH:-}"
fi

# Refuse existing workspaces before touching any build input.
if [[ -n "${FFMPEG_SRC:-}" && (-e "$FFMPEG_SRC" || -L "$FFMPEG_SRC") ]]; then
  echo "FFMPEG_SRC must be a new path; existing checkout preserved: $FFMPEG_SRC" >&2
  exit 1
fi

if ! command -v git >/dev/null; then
  echo "git not found on PATH" >&2
  exit 77
fi
if ! pkg-config --exists libvmaf; then
  echo "libvmaf not found via pkg-config — install libvmaf first or set VMAF_PREFIX" >&2
  exit 77
fi

# A hook caller's index/object-store must never affect this disposable clone.
for git_variable in ${!GIT_@}; do
  unset "$git_variable"
done
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0
ffmpeg_git() {
  command git -c core.hooksPath=/dev/null -c commit.gpgsign=false \
    -c user.name='VMAFx smoke test' -c user.email=smoke@localhost "$@"
}

FFMPEG_SRC="${FFMPEG_SRC:-$(mktemp -d -t vmafx-ffmpeg-smoke.XXXXXXXX)}"
echo "Cloning FFmpeg into $FFMPEG_SRC …"
ffmpeg_git clone --depth 1 --branch "$FFMPEG_SHA" "$FFMPEG_REMOTE" "$FFMPEG_SRC"

echo "Applying patches from $PATCHES_DIR …"
while IFS= read -r line; do
  line="${line%%#*}"
  line="${line// /}"
  [[ -z "$line" ]] && continue
  echo "  → $line"
  ffmpeg_git -C "$FFMPEG_SRC" am --3way "$PATCHES_DIR/$line"
done <"$PATCHES_DIR/series.txt"

echo "Configuring FFmpeg …"
cd "$FFMPEG_SRC"
./configure \
  --disable-doc \
  --disable-debug \
  --disable-programs \
  --enable-ffmpeg \
  --enable-libvmaf \
  --enable-filter=vmaf_pre \
  --enable-gpl

echo "Building FFmpeg …"
make -j"$(nproc)"

echo "Verifying new options …"
./ffmpeg -hide_banner -h filter=libvmaf 2>&1 | grep -q -- 'tiny_model' ||
  {
    echo "libvmaf filter does not advertise tiny_model"
    exit 1
  }
./ffmpeg -hide_banner -h filter=vmaf_pre >/dev/null 2>&1 ||
  {
    echo "vmaf_pre filter not registered"
    exit 1
  }

echo "PASS: ffmpeg-patches smoke ok"

if [[ -z "$KEEP_BUILD" ]]; then
  echo "Cleaning $FFMPEG_SRC (set KEEP_BUILD=1 to keep)."
  rm -rf "$FFMPEG_SRC"
fi
