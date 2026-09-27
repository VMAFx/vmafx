#!/usr/bin/env bash
# Build, stage, stamp and verify the native Linux release bundle.
#
# Runs INSIDE an image built from dev/Containerfile. The release job
# (`build-artifacts` in .github/workflows/supply-chain.yml, ADR-1346) builds
# the `build-deps` stage with scripts/ci/build-dev-container-stage.sh and runs
# this script in it with the checkout mounted as the working directory, as the
# runner's UID (usually without a passwd entry in the image, so HOME is /).
# The Dev Container PR gate rehearses the same invocation. Outside the
# container the first step fails closed (ADR-1102).
#
# Steps, in order (each fails the script):
#   1. assert the process runs in the dev container
#   2. when GITHUB_SHA is set, assert the checkout is that commit: the
#      provenance stamp records GITHUB_SHA as git_commit
#   3. CPU-only optimized Meson build into build/
#   4. stage artifacts/: the libvmaf SONAME chain as regular files, the vmaf
#      CLI, models.tar.gz, and the optional u2netp mirror (ADR-0325)
#   5. stamp artifacts/container-build-provenance.txt
#   6. verify the bundle with verify-native-release-artifacts.sh
#
# Usage (from the repository root):
#   bash scripts/release/build-native-release-artifacts.sh VERSION
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

usage() {
  printf 'Usage: build-native-release-artifacts.sh VERSION\n' >&2
}

# Two settings keep the bundle and the build independent of what the image
# happens to carry:
#   * enable_dnn=disabled: the release ships no ONNX Runtime. build-deps has
#     none, so today this changes nothing; it is explicit so that a stage that
#     does carry ONNX Runtime (libvmaf-build, or a future build-deps) cannot
#     turn `auto` on and make libvmaf.so NEED libonnxruntime.so.1, which the
#     clean-environment verifier cannot resolve.
#   * CCACHE_DISABLE=1: build-deps installs ccache and Meson wraps the
#     compiler in it automatically. Run as a UID with no passwd entry, HOME is
#     / and ccache cannot create its cache directory, so every compile fails
#     with "ccache: error: Permission denied". Disabled, ccache runs the
#     compiler directly and every object is compiled from source.
# The subshell scopes both exports to the build, as the former separate
# workflow step did: the provenance stamp keeps its wall-clock stamped_at.
build_release() (
  SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
  export SOURCE_DATE_EPOCH
  export CCACHE_DISABLE=1
  meson setup build core --buildtype=release -Denable_avx512=true \
    -Denable_cuda=false -Denable_sycl=false -Denable_dnn=disabled
  meson compile -C build
)

# check-container-build.sh --stamp records GITHUB_SHA, when set, as the
# stamp's git_commit, and the build compiles whatever HEAD is. Refuse to build
# when the two differ, so the stamp cannot name a commit that was not built.
# Unset (a local run), the stamp falls back to HEAD itself.
require_checkout_is_github_sha() {
  [[ -n "${GITHUB_SHA:-}" ]] || return 0
  local head
  if ! head="$(git rev-parse --verify "HEAD^{commit}")"; then
    echo "ERROR: cannot resolve the checked-out commit to compare with GITHUB_SHA" >&2
    return 1
  fi
  if [[ "$head" != "$GITHUB_SHA" ]]; then
    printf 'ERROR: checked-out HEAD %s is not GITHUB_SHA %s\n' "$head" "$GITHUB_SHA" >&2
    return 1
  fi
}

# actions/upload-artifact does not preserve symlinks. Materialize every Meson
# link-chain name so the downloaded CLI's DT_NEEDED entry (libvmaf.so.MAJOR)
# exists as a regular release asset.
stage_libvmaf_chain() {
  local -a libvmaf_chain
  mapfile -d '' libvmaf_chain < <(
    find build/src -maxdepth 1 \( -type f -o -type l \) \
      -name 'libvmaf.so*' -print0 | LC_ALL=C sort -z
  )
  if [[ ${#libvmaf_chain[@]} -lt 3 ]]; then
    echo "ERROR: incomplete Meson libvmaf SONAME chain" >&2
    return 1
  fi
  local library
  for library in "${libvmaf_chain[@]}"; do
    cp -L -- "$library" "artifacts/$(basename -- "$library")"
  done
}

stage_models() {
  local archive_epoch
  archive_epoch="$(git show -s --format=%ct HEAD)"
  tar --sort=name --mtime="@$archive_epoch" --owner=0 --group=0 \
    --numeric-owner -cf - model/ | gzip -n >artifacts/models.tar.gz
}

# u2netp_mirror: fork-local mirror of the upstream `xuebinqin/U-2-Net` u2netp
# checkpoint, redistributed under Apache-2.0 §4 NOTICE compliance per
# ADR-0325. The binary is gitignored (model/u2netp_mirror.onnx or .pth, dropped
# by the binary-upload PR). Missing files are a no-op so a release without the
# mirror still succeeds; when present it is hashed into the SLSA subjects and
# signed with everything else, and its license text rides along.
stage_u2netp_mirror() {
  if [ -f model/u2netp_mirror.onnx ]; then
    cp model/u2netp_mirror.onnx artifacts/
    echo "u2netp_mirror: ONNX rewrap staged"
  fi
  if [ -f model/u2netp_mirror.pth ]; then
    cp model/u2netp_mirror.pth artifacts/
    echo "u2netp_mirror: verbatim .pth staged"
  fi
  if [ -f model/u2netp_mirror.onnx ] || [ -f model/u2netp_mirror.pth ]; then
    cp LICENSES/LicenseRef-Apache-2.0-u2netp.txt artifacts/
    echo "u2netp_mirror: license text staged"
  else
    echo "u2netp_mirror: binary not present, skipping (scaffold-only state per ADR-0325)"
  fi
}

main() {
  if [[ $# -ne 1 || -z "$1" ]]; then
    usage
    return 64
  fi
  local version="$1"

  bash scripts/ci/check-container-build.sh --assert
  require_checkout_is_github_sha
  build_release
  mkdir -p artifacts
  stage_libvmaf_chain
  cp build/tools/vmaf artifacts/
  stage_models
  stage_u2netp_mirror
  bash scripts/ci/check-container-build.sh --stamp artifacts
  bash scripts/release/verify-native-release-artifacts.sh artifacts "$version"
}

main "$@"
