#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Install Intel's ocloc offline compiler for icpx SYCL ahead-of-time builds, and
# optionally the rest of the Intel GPU compute stack.
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
# --components picks the package set (ADR-1368):
#   ocloc    (default) ocloc + IGC: a CI host that builds the AOT images and
#            gets the Level Zero loader elsewhere.
#   build    ocloc + IGC + the Level Zero loader and its headers (libze1,
#            libze-dev) at LEVEL_ZERO_VERSION: a builder with no other Level
#            Zero source, such as the oneAPI release image's builder.
#   runtime  the whole GPU stack a host needs to RUN SYCL kernels: the NEO
#            Level Zero GPU driver, OpenCL ICD, gmmlib, IGC and ocloc -- the
#            package set dev/Containerfile installs -- plus the Level Zero loader
#            (libze1) at LEVEL_ZERO_VERSION. The oneAPI release image's final
#            stage uses it; an older NEO there crashed on Arc B580 (ADR-1368).
#
# Debian / Ubuntu x86-64 only: the releases publish amd64 debs. Root containers
# run it directly; CI runners run it through sudo. Set GITHUB_TOKEN to lift the
# anonymous GitHub API rate limit shared by hosted runners.
#
# Usage: install-intel-ocloc.sh [--components ocloc|build|runtime] [repo-root]

set -euo pipefail

components="ocloc"
repo_root=""
while [ $# -gt 0 ]; do
  case "$1" in
    --components=*)
      components="${1#--components=}"
      shift
      ;;
    --components)
      if [ $# -lt 2 ]; then
        echo "::error::install-intel-ocloc: --components requires a value" >&2
        exit 2
      fi
      components="$2"
      shift 2
      ;;
    -*)
      echo "::error::install-intel-ocloc: unknown option '$1'" >&2
      exit 2
      ;;
    *)
      if [ -n "$repo_root" ]; then
        echo "::error::install-intel-ocloc: unexpected argument '$1'" >&2
        exit 2
      fi
      repo_root="$1"
      shift
      ;;
  esac
done
case "$components" in
  ocloc | build | runtime) ;;
  *)
    echo "::error::install-intel-ocloc: unknown component set '$components'" \
      "(expected 'ocloc', 'build' or 'runtime')" >&2
    exit 2
    ;;
esac

repo_root="${repo_root:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}"
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
if [ "$components" != ocloc ] && [ -z "${LEVEL_ZERO_VERSION:-}" ]; then
  echo "::error::install-intel-ocloc: LEVEL_ZERO_VERSION is unset in build-config.env" >&2
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

fetch_args=(--neo-ver "$INTEL_NEO_VERSION" --output-dir "$tmp")
case "$components" in
  ocloc) fetch_args+=(--components ocloc) ;;
  build) fetch_args+=(--components ocloc --level-zero-ver "$LEVEL_ZERO_VERSION" --level-zero-dev) ;;
  runtime) fetch_args+=(--components runtime --level-zero-ver "$LEVEL_ZERO_VERSION") ;;
esac
python3 "$fetcher" "${fetch_args[@]}"
# Local debs install without a package index as long as their dependencies
# (libc, libstdc++) are present; refresh the index only when they are not. The
# runtime set also needs the distro's ocl-icd-libopencl1, so a fresh base image
# takes the second branch.
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

# The fetcher resolved every deb of the set from the pinned releases; prove the
# ones the set exists for are the ones installed, at the pinned versions.
require_package() {
  local package="$1" prefix="$2" actual
  if ! actual="$(dpkg-query -W -f='${Version}' "$package" 2>/dev/null)"; then
    echo "::error::install-intel-ocloc: $package is not installed" >&2
    exit 1
  fi
  case "$actual" in
    "$prefix"*) ;;
    *)
      echo "::error::install-intel-ocloc: $package is $actual, expected $prefix*" >&2
      exit 1
      ;;
  esac
}
if [ "$components" = runtime ]; then
  require_package libze-intel-gpu1 "$INTEL_NEO_VERSION"
  require_package intel-opencl-icd "$INTEL_NEO_VERSION"
fi
if [ "$components" != ocloc ]; then
  require_package libze1 "$LEVEL_ZERO_VERSION"
  if ! $SUDO ldconfig -p | grep -q 'libze_loader\.so\.1 '; then
    echo "::error::install-intel-ocloc: libze1 installed but libze_loader.so.1 does not resolve" >&2
    exit 1
  fi
fi
if [ "$components" = build ]; then
  require_package libze-dev "$LEVEL_ZERO_VERSION"
fi
loader=""
[ "$components" = ocloc ] || loader=", Level Zero loader $LEVEL_ZERO_VERSION"
echo "install-intel-ocloc: '$components' set, ocloc $installed at $(command -v ocloc)$loader"
