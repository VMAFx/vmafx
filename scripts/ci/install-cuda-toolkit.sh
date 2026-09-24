#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Install an exactly pinned CUDA compiler, runtime, or development toolkit from
# NVIDIA's apt repository.
#
# This replaces nvidia/cuda base images (ADR-1306) and Jimver/cuda-toolkit (ADR-1300).
# NVIDIA publishes apt packages on day 1 of a release, whereas nvidia/cuda container
# images frequently lag by weeks or skip point releases entirely (e.g. CUDA 13.4.2
# exists in apt and redist manifests but has no nvidia/cuda OCI images).
#
# Distinct package subsets:
#   builder: exact nvcc + cudart development/runtime packages
#   runtime: exact minimal cudart runtime package
#   full:    exact toolkit release meta-package plus exact core components
#
# Supports execution in:
#   - Root containers (no sudo installed; runs as root directly)
#   - CI runners / host environments (runs with sudo if non-root)
#
# Usage:
#   install-cuda-toolkit.sh [--mode=builder|runtime|full] [repo-root]
#   install-cuda-toolkit.sh --builder [repo-root]
#   install-cuda-toolkit.sh --runtime [repo-root]
#   install-cuda-toolkit.sh --full [repo-root]
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
      if [ $# -lt 2 ]; then
        echo "::error::install-cuda-toolkit: --mode requires a value" >&2
        exit 2
      fi
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
    --full)
      mode="full"
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

if [ "$mode" != "builder" ] && [ "$mode" != "runtime" ] && [ "$mode" != "full" ]; then
  echo "::error::install-cuda-toolkit: unknown mode '$mode'" \
    "(expected 'builder', 'runtime', or 'full')" >&2
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

for required_name in \
  CUDA_VERSION \
  CUDA_APT_LOCK_RELEASE \
  CUDA_APT_PACKAGE \
  CUDA_APT_TOOLKIT_VERSION \
  CUDA_APT_NVCC_VERSION \
  CUDA_APT_CUDART_VERSION; do
  if [ -z "${!required_name:-}" ]; then
    echo "::error::install-cuda-toolkit: ${required_name} is unset in build-config.env" >&2
    exit 2
  fi
done

if [ "$CUDA_APT_LOCK_RELEASE" != "$CUDA_VERSION" ]; then
  echo "::error::install-cuda-toolkit: CUDA_APT_LOCK_RELEASE=${CUDA_APT_LOCK_RELEASE}" \
    "does not bind CUDA_VERSION=${CUDA_VERSION}; refresh the exact NVIDIA package metadata" >&2
  exit 2
fi
if [ "$CUDA_APT_TOOLKIT_VERSION" != "${CUDA_VERSION}-1" ]; then
  echo "::error::install-cuda-toolkit: CUDA_APT_TOOLKIT_VERSION=${CUDA_APT_TOOLKIT_VERSION}" \
    "does not match CUDA_VERSION=${CUDA_VERSION}" >&2
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

for component_name in CUDA_APT_NVCC_VERSION CUDA_APT_CUDART_VERSION; do
  component_version="${!component_name}"
  case "$component_version" in
    "${dotted}."*-*) ;;
    *)
      echo "::error::install-cuda-toolkit: ${component_name}=${component_version}" \
        "does not belong to CUDA series ${dotted}" >&2
      exit 2
      ;;
  esac
done

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

# These two overrides are test seams for the fake-PATH contract tests. Production
# callers leave them unset, which preserves the ordinary host paths exactly.
os_release_file="${VMAFX_CUDA_OS_RELEASE_FILE:-/etc/os-release}"
cuda_prefix="${VMAFX_CUDA_PREFIX:-/usr/local}"
cuda_prefix="${cuda_prefix%/}"

if [ ! -f "$os_release_file" ]; then
  echo "::error::install-cuda-toolkit: OS release file not found: $os_release_file" >&2
  exit 2
fi
if [ -z "$cuda_prefix" ] || [ "${cuda_prefix#/}" = "$cuda_prefix" ]; then
  echo "::error::install-cuda-toolkit: VMAFX_CUDA_PREFIX must be an absolute non-root path" >&2
  exit 2
fi

# ubuntu 26.04 -> ubuntu2604. NVIDIA publishes one repository per distro
# release; a runner image bump that outruns NVIDIA's must fail loudly here
# rather than silently install nothing.
# shellcheck disable=SC1090  # production default is /etc/os-release; tests inject a fixture
. "$os_release_file"
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

cuda_home="${cuda_prefix}/cuda-${dotted}"
cuda_current="${cuda_prefix}/cuda"

require_installed_version() {
  local package="$1" expected="$2" actual
  if ! actual="$(dpkg-query -W -f='${Version}' "$package" 2>/dev/null)"; then
    echo "::error::install-cuda-toolkit: expected package '${package}=${expected}'" \
      "is not installed" >&2
    exit 1
  fi
  if [ "$actual" != "$expected" ]; then
    echo "::error::install-cuda-toolkit: package '${package}' resolved to '${actual}'," \
      "expected exact version '${expected}'" >&2
    exit 1
  fi
}

verify_builder_layout() {
  if [ ! -x "${cuda_home}/bin/nvcc" ]; then
    echo "::error::install-cuda-toolkit: expected nvcc at ${cuda_home}/bin/nvcc after installing" \
      "cuda-nvcc-${series}; the package layout changed" >&2
    exit 1
  fi
  "${cuda_home}/bin/nvcc" --version
}

verify_runtime_layout() {
  if [ ! -e "${cuda_home}/lib64/libcudart.so.${major}" ]; then
    echo "::error::install-cuda-toolkit: expected libcudart under ${cuda_home}/lib64 after installing" \
      "cuda-cudart-${series}; the package layout changed" >&2
    exit 1
  fi

  # Ensure unversioned symlink exists for any dlopen consumer expecting libcudart.so.
  if [ -d "${cuda_home}/targets/x86_64-linux/lib" ] && [ ! -e "${cuda_home}/targets/x86_64-linux/lib/libcudart.so" ]; then
    $SUDO ln -sf "libcudart.so.${major}" "${cuda_home}/targets/x86_64-linux/lib/libcudart.so"
  fi

  if [ -e "$cuda_current" ] && [ ! -L "$cuda_current" ]; then
    echo "::error::install-cuda-toolkit: ${cuda_current} exists but is not a symlink;" \
      "refusing to replace a real directory" >&2
    exit 1
  fi
  # Do not leave Docker ENV CUDA_HOME=/usr/local/cuda pointing at an older
  # parallel install. -n replaces a symlink-to-directory rather than creating
  # a child inside its target; a broken symlink is replaced too.
  $SUDO ln -sfn "${cuda_home}" "$cuda_current"
}

case "$mode" in
  builder)
    $SUDO apt-get install -y -qq --no-install-recommends \
      "cuda-nvcc-${series}=${CUDA_APT_NVCC_VERSION}" \
      "cuda-cudart-dev-${series}=${CUDA_APT_CUDART_VERSION}" \
      "cuda-cudart-${series}=${CUDA_APT_CUDART_VERSION}"
    require_installed_version "cuda-nvcc-${series}" "$CUDA_APT_NVCC_VERSION"
    require_installed_version "cuda-cudart-dev-${series}" "$CUDA_APT_CUDART_VERSION"
    require_installed_version "cuda-cudart-${series}" "$CUDA_APT_CUDART_VERSION"
    verify_builder_layout
    verify_runtime_layout

    if [ -n "${GITHUB_PATH:-}" ]; then
      echo "${cuda_home}/bin" >>"$GITHUB_PATH"
    fi
    if [ -n "${GITHUB_ENV:-}" ]; then
      {
        echo "CUDA_HOME=${cuda_home}"
        echo "CUDA_PATH=${cuda_home}"
      } >>"$GITHUB_ENV"
    fi

    echo "install-cuda-toolkit: CUDA ${CUDA_VERSION} builder" \
      "(nvcc ${CUDA_APT_NVCC_VERSION}, cudart ${CUDA_APT_CUDART_VERSION}) from ${distro}"
    ;;

  runtime)
    $SUDO apt-get install -y -qq --no-install-recommends \
      "cuda-cudart-${series}=${CUDA_APT_CUDART_VERSION}"
    require_installed_version "cuda-cudart-${series}" "$CUDA_APT_CUDART_VERSION"
    verify_runtime_layout

    if [ -n "${GITHUB_ENV:-}" ]; then
      {
        echo "CUDA_HOME=${cuda_home}"
        echo "CUDA_PATH=${cuda_home}"
      } >>"$GITHUB_ENV"
    fi
    echo "install-cuda-toolkit: CUDA ${CUDA_VERSION} runtime" \
      "(cudart ${CUDA_APT_CUDART_VERSION}) from ${distro}"
    ;;

  full)
    $SUDO apt-get install -y -qq --no-install-recommends \
      "${CUDA_APT_PACKAGE}=${CUDA_APT_TOOLKIT_VERSION}" \
      "cuda-nvcc-${series}=${CUDA_APT_NVCC_VERSION}" \
      "cuda-cudart-dev-${series}=${CUDA_APT_CUDART_VERSION}" \
      "cuda-cudart-${series}=${CUDA_APT_CUDART_VERSION}"
    require_installed_version "$CUDA_APT_PACKAGE" "$CUDA_APT_TOOLKIT_VERSION"
    require_installed_version "cuda-nvcc-${series}" "$CUDA_APT_NVCC_VERSION"
    require_installed_version "cuda-cudart-dev-${series}" "$CUDA_APT_CUDART_VERSION"
    require_installed_version "cuda-cudart-${series}" "$CUDA_APT_CUDART_VERSION"
    verify_builder_layout
    verify_runtime_layout
    echo "install-cuda-toolkit: CUDA ${CUDA_VERSION} full development toolkit" \
      "(toolkit ${CUDA_APT_TOOLKIT_VERSION}, nvcc ${CUDA_APT_NVCC_VERSION}," \
      "cudart ${CUDA_APT_CUDART_VERSION}) from ${distro}"
    ;;
esac
