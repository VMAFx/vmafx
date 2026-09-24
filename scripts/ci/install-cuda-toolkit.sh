#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Install the pinned CUDA compiler or runtime subset from NVIDIA's apt repository.
#
# This replaces nvidia/cuda base images (ADR-1306) and Jimver/cuda-toolkit (ADR-1300).
# NVIDIA publishes apt packages on day 1 of a release, whereas nvidia/cuda container
# images frequently lag by weeks or skip point releases entirely (e.g. CUDA 13.4.2
# exists in apt and redist manifests but has no nvidia/cuda OCI images).
#
# Distinct package subsets:
#   builder: cuda-nvcc-<series> + cuda-cudart-dev-<series> (compiler + headers)
#   runtime: cuda-cudart-<series> (minimal runtime shared libraries)
#
# Supports execution in:
#   - Root containers (no sudo installed; runs as root directly)
#   - CI runners / host environments (runs with sudo if non-root)
#
# Usage:
#   install-cuda-toolkit.sh [--mode=builder|runtime] [repo-root]
#   install-cuda-toolkit.sh --builder [repo-root]
#   install-cuda-toolkit.sh --runtime [repo-root]
#
# Defaults to --mode=builder if omitted.

set -euo pipefail

mode="builder"
repo_root=""

while [ $# -gt 0 ]; do
  case "$1" in
    --mode=*)
      mode="${1#--mode=}"
      shift
      ;;
    --mode)
      mode="$2"
      shift 2
      ;;
    --builder)
      mode="builder"
      shift
      ;;
    --runtime)
      mode="runtime"
      shift
      ;;
    -*)
      echo "::error::install-cuda-toolkit: unknown option '$1'" >&2
      exit 2
      ;;
    *)
      if [ -z "$repo_root" ]; then
        repo_root="$1"
      fi
      shift
      ;;
  esac
done

if [ "$mode" != "builder" ] && [ "$mode" != "runtime" ]; then
  echo "::error::install-cuda-toolkit: unknown mode '$mode' (expected 'builder' or 'runtime')" >&2
  exit 2
fi

if [ -z "$repo_root" ]; then
  repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
fi

if [ -f "$repo_root/build-config.env" ]; then
  CONFIG="$repo_root/build-config.env"
elif [ -f "$repo_root" ]; then
  CONFIG="$repo_root"
else
  echo "::error::install-cuda-toolkit: build-config.env not found under $repo_root" >&2
  exit 2
fi

set -a
# shellcheck disable=SC1090  # path is computed, and the file is repo-owned
. "$CONFIG"
set +a

if [ -z "${CUDA_APT_PACKAGE:-}" ]; then
  echo "::error::install-cuda-toolkit: CUDA_APT_PACKAGE is unset in build-config.env" >&2
  exit 2
fi

# cuda-toolkit-13-4 -> 13-4 -> 13.4
series="${CUDA_APT_PACKAGE#cuda-toolkit-}"
if [ "$series" = "$CUDA_APT_PACKAGE" ] || [ -z "$series" ]; then
  echo "::error::install-cuda-toolkit: CUDA_APT_PACKAGE='$CUDA_APT_PACKAGE' is not of the" \
    "form cuda-toolkit-<major>-<minor>" >&2
  exit 2
fi
dotted="${series//-/.}"
major="${series%%-*}"

# Privilege detection: use sudo if non-root, nothing if root.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
  else
    echo "::error::install-cuda-toolkit: non-root execution requires sudo" >&2
    exit 1
  fi
fi

# ubuntu 26.04 -> ubuntu2604. NVIDIA publishes one repository per distro
# release; a runner image bump that outruns NVIDIA's must fail loudly here
# rather than silently install nothing.
# shellcheck disable=SC1091  # provided by the runner image
. /etc/os-release
distro="${ID}${VERSION_ID//./}"
base="https://developer.download.nvidia.com/compute/cuda/repos/${distro}/x86_64"

# Ensure prerequisites for fetching and verifying keyrings exist.
if ! command -v curl >/dev/null 2>&1 || ! dpkg -s ca-certificates >/dev/null 2>&1; then
  $SUDO apt-get update -qq
  $SUDO apt-get install -y -qq --no-install-recommends curl ca-certificates
fi

if ! curl -fsSI "${base}/cuda-keyring_1.1-1_all.deb" >/dev/null; then
  echo "::error::install-cuda-toolkit: NVIDIA publishes no CUDA repository for '${distro}'." \
    "Pin the runner to a release NVIDIA serves, or add the mapping here." >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/cuda-keyring.deb" "${base}/cuda-keyring_1.1-1_all.deb"
$SUDO dpkg -i "$tmp/cuda-keyring.deb"
$SUDO apt-get update -qq

cuda_home="/usr/local/cuda-${dotted}"

case "$mode" in
  builder)
    $SUDO apt-get install -y -qq --no-install-recommends \
      "cuda-nvcc-${series}" "cuda-cudart-dev-${series}"

    if [ ! -x "${cuda_home}/bin/nvcc" ]; then
      echo "::error::install-cuda-toolkit: expected nvcc at ${cuda_home}/bin/nvcc after installing" \
        "cuda-nvcc-${series}; the package layout changed" >&2
      exit 1
    fi

    if [ -n "${GITHUB_PATH:-}" ]; then
      echo "${cuda_home}/bin" >>"$GITHUB_PATH"
    fi
    if [ -n "${GITHUB_ENV:-}" ]; then
      {
        echo "CUDA_HOME=${cuda_home}"
        echo "CUDA_PATH=${cuda_home}"
      } >>"$GITHUB_ENV"
    fi

    "${cuda_home}/bin/nvcc" --version
    echo "install-cuda-toolkit: CUDA ${dotted} builder (${CUDA_VERSION:-unknown}) from ${distro}"
    ;;

  runtime)
    $SUDO apt-get install -y -qq --no-install-recommends \
      "cuda-cudart-${series}"

    # Verify runtime library layout.
    if [ ! -e "${cuda_home}/lib64/libcudart.so.${major}" ] && [ ! -e "/usr/local/cuda/lib64/libcudart.so.${major}" ]; then
      echo "::error::install-cuda-toolkit: expected libcudart under ${cuda_home}/lib64 after installing" \
        "cuda-cudart-${series}; the package layout changed" >&2
      exit 1
    fi

    # Ensure unversioned symlink exists for any dlopen consumer expecting libcudart.so
    if [ -d "${cuda_home}/targets/x86_64-linux/lib" ] && [ ! -e "${cuda_home}/targets/x86_64-linux/lib/libcudart.so" ]; then
      $SUDO ln -sf "libcudart.so.${major}" "${cuda_home}/targets/x86_64-linux/lib/libcudart.so"
    fi

    if [ ! -e /usr/local/cuda ]; then
      $SUDO ln -sf "${cuda_home}" /usr/local/cuda
    fi

    if [ -n "${GITHUB_ENV:-}" ]; then
      {
        echo "CUDA_HOME=${cuda_home}"
        echo "CUDA_PATH=${cuda_home}"
      } >>"$GITHUB_ENV"
    fi
    echo "install-cuda-toolkit: CUDA ${dotted} runtime (${CUDA_VERSION:-unknown}) from ${distro}"
    ;;
esac
