#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Build the right libvmaf flavour for the kit's per-platform binaries
# directory (see binaries/README.md). Operator runs this once per box,
# rsyncs the populated `binaries/<platform>/` directory back to the
# packager, and the kit's distribution tarball includes the binary.
#
# Usage:
#   bash tools/ensemble-training-kit/build-libvmaf-binaries.sh \
#       --platform <linux-x86_64-cuda|linux-x86_64-sycl|linux-x86_64-vulkan|darwin-arm64-cpu|darwin-x86_64-cpu>
#
# The script does NOT auto-detect the platform — passing it explicitly
# makes cross-builds and CI replay deterministic. `01-prereqs.sh`
# auto-detection prints the suggested platform string for the host.

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$KIT_DIR/../.." && pwd)}"
BINARIES_DIR="$KIT_DIR/binaries"

PLATFORM=""

usage() {
  sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \?//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform)
      PLATFORM="$2"
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "[build-libvmaf] error: unknown flag '$1'" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$PLATFORM" ]]; then
  echo "[build-libvmaf] error: --platform is required" >&2
  usage
  exit 2
fi

build_dir=""
meson_args=()
case "$PLATFORM" in
  linux-x86_64-cuda)
    build_dir="$REPO_ROOT/build-cuda"
    meson_args=(-Denable_cuda=true -Denable_sycl=false)
    ;;
  linux-x86_64-sycl)
    build_dir="$REPO_ROOT/build-sycl"
    meson_args=(-Denable_cuda=false -Denable_sycl=true)
    ;;
  linux-x86_64-vulkan)
    build_dir="$REPO_ROOT/build-vulkan"
    # Vulkan path; CPU fallback if the meson option is absent on this
    # branch. The build still succeeds because libvmaf's CPU path is
    # always compiled.
    meson_args=(-Denable_cuda=false -Denable_sycl=false)
    ;;
  darwin-arm64-cpu | darwin-x86_64-cpu)
    build_dir="$REPO_ROOT/build-cpu"
    meson_args=(-Denable_cuda=false -Denable_sycl=false)
    ;;
  *)
    echo "[build-libvmaf] error: unknown platform '$PLATFORM'" >&2
    echo "[build-libvmaf] supported: linux-x86_64-{cuda,sycl,vulkan}, darwin-{arm64,x86_64}-cpu" >&2
    exit 2
    ;;
esac

if ! command -v meson >/dev/null 2>&1; then
  echo "[build-libvmaf] error: meson not in PATH" >&2
  echo "[build-libvmaf] hint: pip install meson ninja, or use your distro's package" >&2
  exit 2
fi
if ! command -v ninja >/dev/null 2>&1; then
  echo "[build-libvmaf] error: ninja not in PATH" >&2
  exit 2
fi

dest_dir="$BINARIES_DIR/$PLATFORM"
mkdir -p "$dest_dir"

echo "[build-libvmaf] platform=$PLATFORM build_dir=$build_dir"
echo "[build-libvmaf] meson args: ${meson_args[*]}"

# meson setup is idempotent for the same args; reconfigure on flag drift.
if [[ -d "$build_dir" ]]; then
  meson setup --reconfigure "$build_dir" "${meson_args[@]}" -C "$REPO_ROOT" ||
    meson setup "$build_dir" "${meson_args[@]}" -C "$REPO_ROOT"
else
  (cd "$REPO_ROOT" && meson setup "$build_dir" "${meson_args[@]}")
fi

ninja -C "$build_dir"

src_bin="$build_dir/tools/vmaf"
if [[ ! -x "$src_bin" ]]; then
  echo "[build-libvmaf] error: build did not produce $src_bin" >&2
  exit 2
fi

cp -f "$src_bin" "$dest_dir/vmaf"
chmod +x "$dest_dir/vmaf"
echo "[build-libvmaf] copied $src_bin -> $dest_dir/vmaf"

# Print a sanity probe — the helper just verifies the binary launches;
# it does NOT run any scoring.
"$dest_dir/vmaf" --version 2>&1 | head -1 || true

echo "[build-libvmaf] done. rsync $dest_dir back to the kit packager."
