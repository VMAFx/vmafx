#!/usr/bin/env bash
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
#
# Install the encoder runtimes the shared fix series' `-qpfile` checks need on
# a hosted Ubuntu runner (issue #3351): libx264 and libaom from apt, SVT-AV1
# from source. This is the CI counterpart of the verified recipe in
# dev/Containerfile, which the production image builds.
#
# Why SVT-AV1 is built from source instead of apt: Ubuntu's libsvtav1-dev
# ships the library and headers but omits the `SvtAv1Enc.pc` pkg-config file
# (a Debian packaging quirk, verified 2026-05-18 against ubuntu:24.04: only
# `aom.pc` and `vpl.pc` land in the pkgconfig directory). FFmpeg's
# `require_pkg_config libsvtav1 SvtAv1Enc ...` configure probe then fails with
# `ERROR: SvtAv1Enc >= 0.9.0 not found using pkg-config` even though the
# library is on disk. Building the pinned upstream release under /usr/local
# installs the headers and the .pc FFmpeg's probe looks for.
#
# Why libaom must be at least 3.13.0: patch 0043 of the shared fix series
# (the `-qpfile` patch) drives its ROI helper in libavcodec/libaomenc.c with
# `aom_roi_map_t` fields (`enabled`, `skip`, `ref_frame`, `delta_qp_enabled`)
# that first shipped in libaom 3.13.0; an older aom fails the FFmpeg build
# with `'aom_roi_map_t' has no member named ...`. Ubuntu 26.04 ships 3.13.1,
# which clears that floor.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
set -a
. "$repo_root/build-config.env"
set +a
: "${SVTAV1_VERSION:?SVTAV1_VERSION is missing from build-config.env}"
: "${SVTAV1_COMMIT:?SVTAV1_COMMIT is missing from build-config.env}"

# SVT-AV1's CMake install may choose lib/ or lib64/ for its .pc file; make both
# visible to FFmpeg's configure probe and to the fail-closed checks below.
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"

# Reuse the runner's cached apt archives / retry config, same as every other
# Ubuntu step in this workflow.
bash scripts/ci/apt-install-cached.sh refresh
# nasm and ninja-build were already installed by the "Install dependencies
# (ubuntu)" step; cmake is the only tool the SVT-AV1 source build needs on
# top of those.
sudo -E apt-get -yq install libx264-dev libaom-dev cmake

# Build SVT-AV1 in $RUNNER_TEMP and throw the tree away afterwards: the only
# deliverables are what `cmake --install` copies under /usr/local.
svt_build="${RUNNER_TEMP:?RUNNER_TEMP is not set}/svt-av1-build"
svt_src="$svt_build/SVT-AV1"
trap 'rm -rf "$svt_build"' EXIT

timeout 300 git clone --depth=1 --branch "v${SVTAV1_VERSION}" \
  https://gitlab.com/AOMediaCodec/SVT-AV1.git "$svt_src"
svt_head="$(git -C "$svt_src" rev-parse HEAD)"
if [ "$svt_head" != "$SVTAV1_COMMIT" ]; then
  echo "SVT-AV1 v${SVTAV1_VERSION} resolved to $svt_head, expected $SVTAV1_COMMIT" >&2
  echo "the tag moved; update SVTAV1_COMMIT only after re-verifying the source" >&2
  exit 1
fi
cmake -S "$svt_src" -B "$svt_build/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_APPS=OFF \
  -DBUILD_TESTING=OFF
cmake --build "$svt_build/build" -j"$(nproc)"
sudo cmake --install "$svt_build/build"
sudo ldconfig

# Fail closed: every encoder the `-qpfile` checks exercise must be resolvable
# through pkg-config, and aom must clear the patch-0043 floor. A silent
# half-install would otherwise surface minutes later as an opaque FFmpeg
# configure failure. Under `set -e` a failing substitution already aborts the
# step; the wrappers below turn that into an explicit, attributable message.
x264_version="$(pkg-config --modversion x264)" || {
  echo "install-ffmpeg-encoders: pkg-config cannot resolve x264 (libx264-dev did not install?)" >&2
  exit 1
}
aom_version="$(pkg-config --modversion aom)" || {
  echo "install-ffmpeg-encoders: pkg-config cannot resolve aom (libaom-dev did not install?)" >&2
  exit 1
}
svtav1_version="$(pkg-config --modversion SvtAv1Enc)" || {
  echo "install-ffmpeg-encoders: pkg-config cannot resolve SvtAv1Enc" >&2
  echo "the SVT-AV1 source build above did not install its .pc under /usr/local" >&2
  exit 1
}

echo "encoder runtime versions: libx264 ${x264_version}, libaom ${aom_version}, libsvtav1 ${svtav1_version}"

case "$aom_version" in
  *.*.*) ;;
  *)
    echo "install-ffmpeg-encoders: cannot parse libaom version '$aom_version'" >&2
    exit 1
    ;;
esac
IFS=. read -r aom_major aom_minor _aom_rest <<<"$aom_version"
if ((aom_major < 3)) || { ((aom_major == 3)) && ((aom_minor < 13)); }; then
  echo "install-ffmpeg-encoders: libaom ${aom_version} is older than the 3.13.0 minimum" >&2
  echo "required by patch 0043 of the shared fix series (aom_roi_map_t ROI fields)" >&2
  exit 1
fi

echo "install-ffmpeg-encoders: all encoder runtimes verified"
