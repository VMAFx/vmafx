#!/usr/bin/env bash
# Copyright 2026 Lusoris
# Copyright 2026 Claude (Anthropic)
# SPDX-License-Identifier: EUPL-1.2
# Install the repository's canonical CUDA compiler/runtime subset on Ubuntu CI.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=/dev/null
. "$repo_root/build-config.env"

if [[ ! "$CUDA_VERSION" =~ ^([0-9]+)\.([0-9]+)\.[0-9]+$ ]]; then
  echo "install-cuda-linux: CUDA_VERSION must be an exact three-part version" >&2
  exit 2
fi
cuda_series="${BASH_REMATCH[1]}-${BASH_REMATCH[2]}"
if [[ "$CUDA_APT_PACKAGE" != "cuda-toolkit-${cuda_series}" ]]; then
  echo "install-cuda-linux: CUDA_APT_PACKAGE does not match CUDA_VERSION" >&2
  exit 2
fi

# GitHub's CUDA lanes run Ubuntu 24.04 or 26.04. Use the matching NVIDIA
# repository instead of a third-party action whose static URL table can lag a
# toolkit release.
# shellcheck source=/dev/null
. /etc/os-release
case "${ID:-}:${VERSION_ID:-}" in
  ubuntu:24.04 | ubuntu:26.04) cuda_distro="ubuntu${VERSION_ID//./}" ;;
  *)
    echo "install-cuda-linux: unsupported host ${ID:-unknown} ${VERSION_ID:-unknown}" >&2
    exit 2
    ;;
esac
case "$(dpkg --print-architecture)" in
  amd64) cuda_arch=x86_64 ;;
  arm64) cuda_arch=sbsa ;;
  *)
    echo "install-cuda-linux: unsupported dpkg architecture" >&2
    exit 2
    ;;
esac

tmp_dir="$(mktemp -d)"
cleanup() {
  if [[ -d "$tmp_dir" ]]; then
    find "$tmp_dir" -depth -delete
  fi
}
trap cleanup EXIT INT TERM
keyring="$tmp_dir/cuda-keyring.deb"
base_url="https://developer.download.nvidia.com/compute/cuda/repos/${cuda_distro}/${cuda_arch}"
curl --fail --silent --show-error --location --retry 5 --retry-delay 10 \
  "$base_url/cuda-keyring_1.1-1_all.deb" --output "$keyring"
sudo dpkg -i "$keyring"
sudo apt-get update
sudo -E apt-get -yq install --no-install-recommends \
  "cuda-nvcc-${cuda_series}" "cuda-cudart-dev-${cuda_series}"

cuda_major_minor="${CUDA_VERSION%.*}"
cuda_path="/usr/local/cuda-${cuda_major_minor}"
if [[ ! -x "$cuda_path/bin/nvcc" ]]; then
  echo "install-cuda-linux: nvcc was not installed under $cuda_path" >&2
  exit 1
fi
if [[ -n "${GITHUB_ENV:-}" ]]; then
  printf 'CUDA_PATH=%s\n' "$cuda_path" >>"$GITHUB_ENV"
fi
if [[ -n "${GITHUB_PATH:-}" ]]; then
  printf '%s\n' "$cuda_path/bin" >>"$GITHUB_PATH"
fi
"$cuda_path/bin/nvcc" --version
