#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Install Intel's ocloc offline compiler for icpx SYCL ahead-of-time builds.
#
# icpx compiles the spir64_gen AOT images that `sycl_icpx_aot_targets` asks for
# by running `ocloc`, and the Linux oneAPI compiler does not ship it; without it
# core/src/meson.build refuses to configure (ADR-1360). This installs
# intel-ocloc and the two IGC packages it loads at run time from the
# intel/compute-runtime release pinned as INTEL_NEO_VERSION in build-config.env,
# the release dev/Containerfile takes the GPU runtime from. The download goes
# through dev/scripts/fetch-intel-neo.py, which checks every deb against the
# SHA-256 sums the release publishes.
#
# Debian / Ubuntu x86-64 only: the release publishes amd64 debs. Root containers
# run it directly; CI runners run it through sudo. Set GITHUB_TOKEN to lift the
# anonymous GitHub API rate limit shared by hosted runners.
#
# Usage: install-intel-ocloc.sh [repo-root]

set -euo pipefail

repo_root="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}"
config="$repo_root/build-config.env"
fetcher="$repo_root/dev/scripts/fetch-intel-neo.py"

if [ ! -f "$config" ] || [ ! -f "$fetcher" ]; then
  echo "::error::install-intel-ocloc: $config or $fetcher not found" >&2
  exit 2
fi
# shellcheck disable=SC1090  # path is computed, and the file is repo-owned
. "$config"
if [ -z "${INTEL_NEO_VERSION:-}" ]; then
  echo "::error::install-intel-ocloc: INTEL_NEO_VERSION is unset in build-config.env" >&2
  exit 2
fi
if ! command -v python3 >/dev/null 2>&1 || ! command -v apt-get >/dev/null 2>&1; then
  echo "::error::install-intel-ocloc: needs python3 and apt-get (Debian / Ubuntu)" >&2
  exit 2
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  if ! command -v sudo >/dev/null 2>&1; then
    echo "::error::install-intel-ocloc: non-root execution requires sudo" >&2
    exit 1
  fi
  SUDO="sudo"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

python3 "$fetcher" --neo-ver "$INTEL_NEO_VERSION" --components ocloc --output-dir "$tmp"
# Local debs install without a package index as long as their dependencies
# (libc, libstdc++) are present; refresh the index only when they are not.
if ! $SUDO apt-get install -y --no-install-recommends "$tmp"/*.deb; then
  $SUDO apt-get update -qq
  $SUDO apt-get install -y --no-install-recommends "$tmp"/*.deb
fi
# The IGC debs put libigc.so.2 and libigdfcl.so.2 in /usr/local/lib without an
# ldconfig trigger; until the cache is rebuilt ocloc fails with "Loading of IGC
# library has failed".
$SUDO ldconfig

# The package installs ocloc-<major>.<minor>.<n> and registers `ocloc` as an
# alternative; icpx looks for exactly `ocloc` on PATH.
if ! command -v ocloc >/dev/null 2>&1; then
  echo "::error::install-intel-ocloc: intel-ocloc installed but no ocloc on PATH" >&2
  exit 1
fi
installed="$(ocloc query OCL_DRIVER_VERSION 2>/dev/null | tail -n 1 | tr -d '[:space:]')"
if [ "$installed" != "$INTEL_NEO_VERSION" ]; then
  echo "::error::install-intel-ocloc: ocloc reports '$installed'," \
    "build-config.env pins INTEL_NEO_VERSION=$INTEL_NEO_VERSION" >&2
  exit 1
fi
# `query` does not load IGC; compiling does. Prove the compiler libraries load.
printf '__kernel void k(__global float *p) { p[get_global_id(0)] *= 2.0f; }\n' >"$tmp/k.cl"
if ! (cd "$tmp" && ocloc compile -q -file k.cl -device bmg-g21 >"$tmp/ocloc.log" 2>&1); then
  cat "$tmp/ocloc.log" >&2
  echo "::error::install-intel-ocloc: ocloc cannot compile; IGC did not load" >&2
  exit 1
fi
echo "install-intel-ocloc: ocloc $installed at $(command -v ocloc)"
