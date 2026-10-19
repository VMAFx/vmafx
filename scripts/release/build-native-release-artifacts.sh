#!/usr/bin/env bash
# Build, stage, stamp and verify the native Linux release bundle.
#
# Runs INSIDE an image built from dev/Containerfile. The release job
# (`build-artifacts` in .github/workflows/supply-chain.yml, ADR-1346, ADR-1354)
# builds the Debian 13 `release-build` stage with
# scripts/ci/build-dev-container-stage.sh and runs this script in it with the
# checkout mounted as the working directory, as the runner's UID (usually
# without a passwd entry in the image, so HOME is /).
# The Dev Container PR gate rehearses the same invocation. Outside the
# container the first step fails closed (ADR-1102).
#
# Steps, in order (each fails the script):
#   1. assert the process runs in the dev container
#   2. when GITHUB_SHA is set, assert the checkout is that commit: the
#      provenance stamp records GITHUB_SHA as git_commit
#   3. assert patchelf is available, before anything is compiled
#   4. CPU-only optimized Meson build into build/
#   5. stage artifacts/: the libvmaf SONAME chain as regular files, the vmaf
#      CLI with its RUNPATH rewritten to $ORIGIN, models.tar.gz (with its own
#      licenses/ directory, checked by licensing.py, ADR-1513), and the
#      optional u2netp mirror (ADR-0325)
#   6. stamp artifacts/container-build-provenance.txt
#   7. write and check the bundle's notices (licensing.py, artifact kind
#      release-native): THIRD_PARTY_NOTICES.txt and licenses.tar.gz
#   8. verify the bundle with verify-native-release-artifacts.sh
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

# Three settings keep the bundle and the build independent of what the image
# happens to carry:
#   * enable_dnn=disabled: the release ships no ONNX Runtime. release-build
#     has none, so today this changes nothing; it is explicit so that a stage
#     that does carry ONNX Runtime (libvmaf-build, or a future release-build)
#     cannot turn `auto` on and make libvmaf.so NEED libonnxruntime.so.1,
#     which the clean-environment verifier cannot resolve.
#   * enable_tests=false: the release ships libvmaf.so* and vmaf, not the unit
#     tests, which other CI jobs build and run. Debian 13's GCC 14.2 crashed
#     at random while LTO-linking the ~150 test executables (an internal
#     compiler error, once reported as heap corruption in lto1) in two of
#     three full builds, so building them would make the release fail by
#     chance (ADR-1354). docker/Dockerfile.production, Dockerfile.node and
#     Dockerfile.controller build libvmaf on the same base with it too.
#   * CCACHE_DISABLE=1: build-deps installs ccache and Meson wraps the
#     compiler in it automatically. Run as a UID with no passwd entry, HOME is
#     / and ccache cannot create its cache directory, so every compile fails
#     with "ccache: error: Permission denied". release-build has no ccache;
#     the setting keeps any stage that does from breaking the release build.
# The subshell scopes both exports to the build, as the former separate
# workflow step did: the provenance stamp keeps its wall-clock stamped_at.
build_release() (
  SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
  export SOURCE_DATE_EPOCH
  export CCACHE_DISABLE=1
  meson setup build core --buildtype=release -Denable_avx512=true \
    -Denable_cuda=false -Denable_sycl=false -Denable_dnn=disabled \
    -Denable_tests=false
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
# link-chain name so the downloaded CLI's DT_NEEDED entries (libvmaf.so.MAJOR
# and libvmafx.so.MAJOR) exist as regular release assets. Since the library
# split (ADR-2094) the CLI and the compat libvmaf.so.3 both need libvmafx.so.1,
# so the bundle carries both chains.
stage_library_chain() {
  local name="$1"
  local -a chain
  mapfile -d '' chain < <(
    find build/src -maxdepth 1 \( -type f -o -type l \) \
      -name "$name.so*" -print0 | LC_ALL=C sort -z
  )
  if [[ ${#chain[@]} -lt 3 ]]; then
    echo "ERROR: incomplete Meson $name SONAME chain" >&2
    return 1
  fi
  local library
  for library in "${chain[@]}"; do
    cp -L -- "$library" "artifacts/$(basename -- "$library")"
  done
}

stage_libvmaf_chain() {
  stage_library_chain libvmaf
  stage_library_chain libvmafx
}

require_patchelf() {
  if ! command -v patchelf >/dev/null; then
    echo "ERROR: patchelf is required to stage the vmaf CLI; dev/Containerfile installs it in release-build" >&2
    return 1
  fi
}

# Meson links build/tools/vmaf with the build-tree RUNPATH `$ORIGIN/../src`,
# which finds libvmaf only inside build/. The bundle ships the CLI next to its
# libvmaf chain, so the staged copy gets RUNPATH exactly `$ORIGIN` and runs
# without LD_LIBRARY_PATH (T-RELEASE-NATIVE-RUNPATH-BUILD-TREE-2026-09-27).
# `meson install` is no substitute: it removes the build RUNPATH and writes the
# target's install_rpath, which is empty, and setting one would change every
# installed vmaf rather than this bundle. The build tree keeps its own RUNPATH.
stage_cli() {
  cp -- build/tools/vmaf artifacts/vmaf
  # shellcheck disable=SC2016 # $ORIGIN is for the dynamic loader, not the shell.
  patchelf --set-rpath '$ORIGIN' artifacts/vmaf
}

LICENSING=tools/rc1-tester/image/licensing.py
LICENCE_WORK=build/release-licences

# The notices of one staged tree (ADR-1503 rule 4, ADR-1513): written by
# licensing.py from tools/rc1-tester/image/licensing.json, then checked; the
# check fails on a file no component records or a missing licence text.
write_and_check_notices() {
  local kind="$1" root="$2" scan="$3"
  python3 "$LICENSING" notices --artifact "$kind" --root "$root" --repo . \
    --build-scan "$scan" --texts "$LICENCE_WORK/no-fetched-texts" \
    --source-commit "$(git rev-parse HEAD)" --tag "v${RELEASE_VERSION}"
  python3 "$LICENSING" check --artifact "$kind" --root "$root" --repo . \
    --build-scan "$scan" --python-version none
}

# Release tarballs are gzip at level 9 (ADR-1591): gzip is the one compressor every
# Linux tar extracts without an extra package (xz-utils and zstd are not Essential on
# Debian), and -n keeps the name and time out of the header so the archive is
# byte-reproducible.
#
# models.tar.gz holds model/ and the notices of the models in it (licenses/).
stage_models() {
  local archive_epoch stage="$LICENCE_WORK/models"
  archive_epoch="$(git show -s --format=%ct HEAD)"
  mkdir -p "$stage" "$LICENCE_WORK/no-fetched-texts"
  cp -a model "$stage/model"
  printf '{"schema_version": 1, "licences": [], "files": [], "system_inputs": 0}\n' \
    >"$LICENCE_WORK/no-compiled-sources.json"
  write_and_check_notices release-models "$stage" "$LICENCE_WORK/no-compiled-sources.json"
  tar --sort=name --mtime="@$archive_epoch" --owner=0 --group=0 \
    --numeric-owner -C "$stage" -cf - model licenses | gzip -9n >artifacts/models.tar.gz
}

# THIRD_PARTY_NOTICES.txt and licenses.tar.gz for the release files: the
# licences of every file the build compiled (ninja -t deps and SPDX headers),
# checked against a copy of the staged artifacts.
stage_licences() {
  local archive_epoch stage="$LICENCE_WORK/release"
  archive_epoch="$(git show -s --format=%ct HEAD)"
  python3 "$LICENSING" scan-build --build build --repo . --out "$LICENCE_WORK/vmafx-sources.json"
  mkdir -p "$stage"
  cp -a artifacts/. "$stage/"
  write_and_check_notices release-native "$stage" "$LICENCE_WORK/vmafx-sources.json"
  cp "$stage/licenses/THIRD_PARTY_NOTICES.txt" artifacts/THIRD_PARTY_NOTICES.txt
  tar --sort=name --mtime="@$archive_epoch" --owner=0 --group=0 \
    --numeric-owner -C "$stage" -cf - licenses | gzip -9n >artifacts/licenses.tar.gz
}

# u2netp_mirror: fork-local mirror of the upstream `xuebinqin/U-2-Net` u2netp
# checkpoint, redistributed under Apache-2.0 §4 NOTICE compliance per
# ADR-0325. The binary is gitignored (model/u2netp_mirror.onnx or .pth, dropped
# by the binary-upload PR). Missing files are a no-op so a release without the
# mirror still succeeds; when present it is hashed into the provenance subjects and
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
  RELEASE_VERSION="$version"

  bash scripts/ci/check-container-build.sh --assert
  require_checkout_is_github_sha
  require_patchelf
  build_release
  mkdir -p artifacts
  stage_libvmaf_chain
  stage_cli
  stage_models
  stage_u2netp_mirror
  bash scripts/ci/check-container-build.sh --stamp artifacts
  stage_licences
  bash scripts/release/verify-native-release-artifacts.sh artifacts "$version"
}

main "$@"
