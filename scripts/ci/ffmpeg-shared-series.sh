#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# ffmpeg-shared-series.sh — fetch, verify and apply the shared FFmpeg fix
# series (VMAFx/ffmpeg-patches) that every VMAFx FFmpeg build applies before
# ffmpeg-patches/series.txt (ADR-3143).
#
# The series is a release tarball pinned in build-config.env:
#
#   FFMPEG_FIX_SERIES_REPO     URL of the GitHub repository that publishes it
#   FFMPEG_FIX_SERIES_TAG      release tag
#   FFMPEG_FIX_SERIES_SHA256   sha256 of ffmpeg-patches-<tag>.tar.gz
#
# This script is the one implementation of "apply the shared series": the
# patch-stack replay (scripts/ci/ffmpeg_patch_stack.py), the smoke build
# (ffmpeg-patches/test/build-and-run.sh), the FFmpeg workflows and the four
# container builds all call it. It needs bash, git, tar, a sha256 tool and
# curl or wget; nothing else, so it runs in a builder stage, on macOS and in
# the MSYS2 shell of the Windows leg.
#
# Usage:
#   ffmpeg-shared-series.sh fetch DEST
#       Download (or reuse the cached) tarball, verify it and unpack it into
#       the new directory DEST, which then holds series.txt and patches/.
#   ffmpeg-shared-series.sh apply [--method am|apply] TREE
#       fetch, then apply the series to the FFmpeg tree TREE in series.txt
#       order. `am` (default) commits each patch with `git am --3way` and
#       requires TREE's HEAD to be FFMPEG_COMMIT; `apply` uses `git apply`
#       for a tree that is not a Git checkout (a release archive).
#
# Verification, always fail closed:
#   1. the tarball's sha256 equals FFMPEG_FIX_SERIES_SHA256;
#   2. its base.env names FFMPEG_TAG and FFMPEG_COMMIT of build-config.env;
#   3. the signature of the release's SHA256SUMS, per FFMPEG_FIX_SERIES_VERIFY:
#        auto    (default) with cosign when cosign is installed, else not
#                checked, and the output says which;
#        cosign  required: a missing cosign is an error;
#        sha256  not checked.
#
# Environment:
#   BUILD_CONFIG              build-config.env to read (default: beside this
#                             script, then the repository root)
#   FFMPEG_FIX_SERIES_URL     tarball location instead of the GitHub release
#                             (a file:// URL in the tests)
#   FFMPEG_FIX_SERIES_CACHE   where the verified tarball is kept (default:
#                             the user cache); `none` keeps nothing, for an
#                             image build. The series is unpacked afresh from
#                             the verified tarball on every run, into a
#                             directory of that run, so concurrent runs share
#                             the download and nothing else.
#
# Exit: 0 success; 1 a verification failed or a patch did not apply; 2 usage,
# a missing tool or missing configuration.
set -euo pipefail
export LC_ALL=C

readonly PROG="ffmpeg-shared-series"
readonly DOWNLOAD_SECONDS=120
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# What this run created and removes on exit.
work_dir=""
temporary_cache=""

cleanup() {
  if [ -n "$work_dir" ] && [ -d "$work_dir" ]; then
    rm -rf -- "$work_dir"
  fi
  if [ -n "$temporary_cache" ] && [ -d "$temporary_cache" ]; then
    rm -rf -- "$temporary_cache"
  fi
}
trap cleanup EXIT

die() { # die STATUS MESSAGE...
  local status=$1
  shift
  printf '%s: %s\n' "$PROG" "$*" >&2
  exit "$status"
}

note() { printf '%s: %s\n' "$PROG" "$*" >&2; }

load_config() {
  local config="${BUILD_CONFIG:-}" candidate
  if [ -z "$config" ]; then
    for candidate in "$script_dir/build-config.env" "$script_dir/../../build-config.env"; do
      if [ -f "$candidate" ]; then
        config="$candidate"
        break
      fi
    done
  fi
  if [ -n "$config" ]; then
    [ -f "$config" ] || die 2 "build configuration not found: $config"
    # shellcheck disable=SC1090  # the repository's own pin file
    . "$config"
  fi
  local key
  for key in FFMPEG_FIX_SERIES_REPO FFMPEG_FIX_SERIES_TAG FFMPEG_FIX_SERIES_SHA256 \
    FFMPEG_TAG FFMPEG_COMMIT; do
    [ -n "${!key:-}" ] || die 2 "$key is not set (build-config.env pins it)"
  done
  case "$FFMPEG_FIX_SERIES_SHA256" in
    *[!0-9a-f]* | "") die 2 "FFMPEG_FIX_SERIES_SHA256 is not a lowercase sha256" ;;
  esac
  [ "${#FFMPEG_FIX_SERIES_SHA256}" -eq 64 ] ||
    die 2 "FFMPEG_FIX_SERIES_SHA256 is not a lowercase sha256"
  case "$FFMPEG_FIX_SERIES_REPO" in
    https://*) ;;
    *) die 2 "FFMPEG_FIX_SERIES_REPO must be an https:// repository URL" ;;
  esac
  release_url="${FFMPEG_FIX_SERIES_REPO}/releases/download/${FFMPEG_FIX_SERIES_TAG}"
  tarball_name="ffmpeg-patches-${FFMPEG_FIX_SERIES_TAG}.tar.gz"
  tarball_url="${FFMPEG_FIX_SERIES_URL:-${release_url}/${tarball_name}}"
}

sha256_of() { # sha256_of FILE
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    die 2 "sha256sum or shasum is required"
  fi
}

download() { # download URL DESTINATION
  local url=$1 destination=$2
  case "$url" in
    file://*)
      cp -- "${url#file://}" "$destination" || die 1 "cannot read $url"
      ;;
    https://*)
      if command -v curl >/dev/null 2>&1; then
        curl --fail --silent --show-error --location --retry 3 \
          --max-time "$DOWNLOAD_SECONDS" --output "$destination" "$url" ||
          die 1 "download failed: $url"
      elif command -v wget >/dev/null 2>&1; then
        wget --quiet --tries=3 --timeout="$DOWNLOAD_SECONDS" --output-document="$destination" "$url" ||
          die 1 "download failed: $url"
      else
        die 2 "curl or wget is required to download $url"
      fi
      ;;
    *) die 2 "unsupported URL (https:// or file:// only): $url" ;;
  esac
}

# The release signs SHA256SUMS; the tarball must be the one it lists.
verify_signature() { # verify_signature TARBALL WORK
  local tarball=$1 work=$2 mode="${FFMPEG_FIX_SERIES_VERIFY:-auto}" identity listed
  case "$mode" in
    auto | cosign | sha256) ;;
    *) die 2 "FFMPEG_FIX_SERIES_VERIFY must be auto, cosign or sha256: $mode" ;;
  esac
  if [ "$mode" = sha256 ]; then
    signature_status="not checked (FFMPEG_FIX_SERIES_VERIFY=sha256)"
    return 0
  fi
  if ! command -v cosign >/dev/null 2>&1; then
    [ "$mode" = auto ] || die 2 "cosign is required (FFMPEG_FIX_SERIES_VERIFY=cosign) and not installed"
    signature_status="not checked (cosign is not installed)"
    return 0
  fi
  if [ -n "${FFMPEG_FIX_SERIES_URL:-}" ]; then
    [ "$mode" = auto ] || die 2 "FFMPEG_FIX_SERIES_URL overrides the release; its signature cannot be checked"
    signature_status="not checked (FFMPEG_FIX_SERIES_URL overrides the release)"
    return 0
  fi
  download "${release_url}/SHA256SUMS" "$work/SHA256SUMS"
  download "${release_url}/SHA256SUMS.sigstore.json" "$work/SHA256SUMS.sigstore.json"
  identity="${FFMPEG_FIX_SERIES_REPO}/.github/workflows/release-build.yml@refs/tags/${FFMPEG_FIX_SERIES_TAG}"
  cosign verify-blob --bundle "$work/SHA256SUMS.sigstore.json" \
    --certificate-identity "$identity" \
    --certificate-oidc-issuer https://token.actions.githubusercontent.com \
    "$work/SHA256SUMS" >/dev/null 2>"$work/cosign.log" ||
    die 1 "cosign refused SHA256SUMS of ${FFMPEG_FIX_SERIES_TAG}: $(tr '\n' ' ' <"$work/cosign.log")"
  listed="$(awk -v name="$tarball_name" '$2 == name || $2 == "*" name {print $1}' "$work/SHA256SUMS")"
  [ "$listed" = "$(sha256_of "$tarball")" ] ||
    die 1 "the signed SHA256SUMS does not list ${tarball_name} with the pinned sha256"
  signature_status="cosign verified ($identity)"
}

# A member outside the unpack directory would be written wherever it names.
check_members() { # check_members TARBALL
  local member
  while IFS= read -r member; do
    case "$member" in
      /* | ../* | */../* | */..) die 1 "tarball member leaves the archive root: $member" ;;
    esac
  done < <(tar -tzf "$1") || die 1 "tarball cannot be listed"
}

base_value() { # base_value KEY FILE — base.env is data, never sourced
  sed -n "s/^$1=//p" "$2" | tr -d '\r"' | head -n 1
}

# fetch_series — sets series_dir to the verified, unpacked series.
fetch_series() {
  local cache actual work
  if [ "${FFMPEG_FIX_SERIES_CACHE:-}" = none ]; then
    temporary_cache="$(mktemp -d "${TMPDIR:-/tmp}/ffmpeg-fix-series.XXXXXX")"
    cache="$temporary_cache"
  else
    cache="${FFMPEG_FIX_SERIES_CACHE:-${XDG_CACHE_HOME:-${HOME:-/tmp}/.cache}/vmafx/ffmpeg-fix-series}"
    cache="$cache/${FFMPEG_FIX_SERIES_TAG}-${FFMPEG_FIX_SERIES_SHA256:0:12}"
    mkdir -p "$cache" || die 2 "cannot create $cache"
  fi
  work_dir="$(mktemp -d "$cache/work.XXXXXX")"
  work="$work_dir"
  local tarball="$cache/$tarball_name" source_note="cache"
  if [ ! -f "$tarball" ] || [ "$(sha256_of "$tarball")" != "$FFMPEG_FIX_SERIES_SHA256" ]; then
    download "$tarball_url" "$work/$tarball_name"
    actual="$(sha256_of "$work/$tarball_name")"
    [ "$actual" = "$FFMPEG_FIX_SERIES_SHA256" ] ||
      die 1 "sha256 of $tarball_url is $actual, build-config.env pins $FFMPEG_FIX_SERIES_SHA256"
    mv -f -- "$work/$tarball_name" "$tarball"
    source_note="downloaded"
  fi
  verify_signature "$tarball" "$work"
  check_members "$tarball"
  mkdir "$work/tree"
  tar -xzf "$tarball" -C "$work/tree" --strip-components=1 || die 1 "tarball cannot be unpacked"
  local base_tag base_commit
  [ -f "$work/tree/base.env" ] && [ -f "$work/tree/series.txt" ] ||
    die 1 "tarball has no base.env or series.txt"
  base_tag="$(base_value FFMPEG_TAG "$work/tree/base.env")"
  base_commit="$(base_value FFMPEG_COMMIT "$work/tree/base.env")"
  if [ "$base_tag" != "$FFMPEG_TAG" ] || [ "$base_commit" != "$FFMPEG_COMMIT" ]; then
    die 1 "shared series ${FFMPEG_FIX_SERIES_TAG} targets ${base_tag} (${base_commit}), build-config.env pins ${FFMPEG_TAG} (${FFMPEG_COMMIT}); pin a series release for that FFmpeg"
  fi
  series_dir="$work/tree"
  note "${FFMPEG_FIX_SERIES_REPO} ${FFMPEG_FIX_SERIES_TAG} (${source_note}): sha256 matches the pin; base ${base_tag}; signature ${signature_status}"
}

series_patches() { # prints the patch names of series.txt, in order
  local line
  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%%#*}"
    line="${line//$'\r'/}"
    line="${line// /}"
    [ -n "$line" ] || continue
    case "$line" in
      */* | .* | *[!A-Za-z0-9._-]*) die 1 "unexpected series.txt entry: $line" ;;
    esac
    [ -f "$series_dir/patches/$line" ] || die 1 "series.txt names a missing patch: $line"
    printf '%s\n' "$line"
  done <"$series_dir/series.txt"
}

apply_series() { # apply_series METHOD TREE
  local method=$1 tree=$2 head name count=0 names
  [ -d "$tree" ] || die 2 "not a directory: $tree"
  command -v git >/dev/null 2>&1 || die 2 "git is required"
  if [ "$method" = am ]; then
    head="$(git -C "$tree" rev-parse HEAD 2>/dev/null)" || die 1 "$tree is not a Git checkout (use --method apply)"
    [ "$head" = "$FFMPEG_COMMIT" ] ||
      die 1 "$tree is at $head, the series applies to ${FFMPEG_TAG} (${FFMPEG_COMMIT})"
  fi
  names="$(series_patches)"
  [ -n "$names" ] || die 1 "the shared series is empty"
  while IFS= read -r name; do
    note "applying ${FFMPEG_FIX_SERIES_TAG}: $name"
    if [ "$method" = am ]; then
      git -C "$tree" -c core.hooksPath=/dev/null -c commit.gpgsign=false \
        -c user.name='VMAFx shared FFmpeg series' -c user.email=ffmpeg-fix-series@localhost \
        am --3way "$series_dir/patches/$name" ||
        die 1 "$name did not apply to $tree"
    else
      git -C "$tree" apply "$series_dir/patches/$name" || die 1 "$name did not apply to $tree"
    fi
    count=$((count + 1))
  done <<<"$names"
  note "applied $count patches of ${FFMPEG_FIX_SERIES_TAG} to $tree (git $method)"
}

main() {
  local command="${1:-}" method=am
  [ -n "$command" ] || die 2 "usage: $0 fetch DEST | apply [--method am|apply] TREE"
  shift
  case "$command" in
    fetch)
      [ "$#" -eq 1 ] || die 2 "fetch takes the directory to unpack into"
      [ ! -e "$1" ] || die 2 "fetch needs a new directory, this exists: $1"
      load_config
      fetch_series
      mv -- "$series_dir" "$1" || die 1 "cannot create $1"
      note "unpacked into $1"
      ;;
    apply)
      if [ "${1:-}" = "--method" ]; then
        method="${2:-}"
        shift 2 || die 2 "--method needs am or apply"
      fi
      case "$method" in am | apply) ;; *) die 2 "--method must be am or apply: $method" ;; esac
      [ "$#" -eq 1 ] || die 2 "apply takes one FFmpeg tree"
      load_config
      fetch_series
      apply_series "$method" "$1"
      ;;
    *) die 2 "unknown command: $command" ;;
  esac
}

main "$@"
