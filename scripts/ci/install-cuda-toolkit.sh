#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Install the pinned CUDA compiler subset from NVIDIA's apt repository.
#
# This replaces Jimver/cuda-toolkit on the Linux legs. That action carries its
# own table of downloadable CUDA releases, so the fork could only pin a version
# the action already knew: v0.2.36, its newest release as of 2026-08-02, tops
# out at 13.3.1 (src/links/linux-links.ts), and CUDA 13.4.1 has no entry at all.
# renovate.json recorded that coupling as "a CUDA bump the action cannot serve
# needs the action raised first", which in practice meant the CUDA release pin
# was gated on a third party cutting a release. NVIDIA's own apt repository has
# no such lag, and the fork already carries the apt package name as a
# first-class pin in build-config.env because dev/Containerfile installs from
# it. This makes the CI legs use the same source.
#
# Installs the same subset the action did -- the action's
# sub-packages '["nvcc", "cudart-dev"]' are cuda-nvcc-<series> and
# cuda-cudart-dev-<series> -- rather than the multi-gigabyte cuda-toolkit
# metapackage, so runner time and disk are unchanged.
#
# The CUDA release comes from build-config.env and nowhere else; see ADR-1285
# and scripts/ci/check-cuda-pin-lockstep.py, which holds every spelling of it
# in step.
#
# Usage: install-cuda-toolkit.sh [repo-root]

set -euo pipefail

REPO_ROOT="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}"
CONFIG="$REPO_ROOT/build-config.env"

if [ ! -f "$CONFIG" ]; then
  echo "::error::install-cuda-toolkit: $CONFIG not found" >&2
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

# ubuntu 26.04 -> ubuntu2604. NVIDIA publishes one repository per distro
# release; a runner image bump that outruns NVIDIA's must fail loudly here
# rather than silently install nothing.
# shellcheck disable=SC1091  # provided by the runner image
. /etc/os-release
distro="${ID}${VERSION_ID//./}"
base="https://developer.download.nvidia.com/compute/cuda/repos/${distro}/x86_64"

if ! curl -fsSI "${base}/cuda-keyring_1.1-1_all.deb" >/dev/null; then
  echo "::error::install-cuda-toolkit: NVIDIA publishes no CUDA repository for '${distro}'." \
    "Pin the runner to a release NVIDIA serves, or add the mapping here." >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/cuda-keyring.deb" "${base}/cuda-keyring_1.1-1_all.deb"
sudo dpkg -i "$tmp/cuda-keyring.deb"
sudo apt-get update -qq
sudo apt-get install -y -qq --no-install-recommends \
  "cuda-nvcc-${series}" "cuda-cudart-dev-${series}"

cuda_home="/usr/local/cuda-${dotted}"
if [ ! -x "${cuda_home}/bin/nvcc" ]; then
  echo "::error::install-cuda-toolkit: expected nvcc at ${cuda_home}/bin/nvcc after installing" \
    "cuda-nvcc-${series}; the package layout changed" >&2
  exit 1
fi

echo "${cuda_home}/bin" >>"${GITHUB_PATH:-/dev/stdout}"
{
  echo "CUDA_HOME=${cuda_home}"
  echo "CUDA_PATH=${cuda_home}"
} >>"${GITHUB_ENV:-/dev/stdout}"

"${cuda_home}/bin/nvcc" --version
echo "install-cuda-toolkit: CUDA ${dotted} (${CUDA_VERSION:-unknown}) from ${distro}"
