#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# One pinned shfmt install path for the Ubuntu and Fedora setup scripts (issue
# 3101, HISS-19). The version and per-architecture release digests live in
# build-config.env. `--resolve <machine>` prints "<asset> <sha256>" without
# downloading, which the tests use to bind each digest to its architecture.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${VMAFX_REPO_ROOT:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}"
CONFIG="$REPO_ROOT/build-config.env"

read_pin() {
  local name="$1"
  local value
  value="$(sed -n "s/^${name}=\"\([^\"]*\)\"$/\1/p" "$CONFIG" | head -n1)"
  if [[ -z "$value" ]]; then
    echo "install-shfmt.sh: ${CONFIG} declares no ${name}" >&2
    exit 2
  fi
  printf '%s' "$value"
}

mode=install
if [[ "${1:-}" == "--resolve" ]]; then
  mode=resolve
  shift
fi
machine="${1:-$(uname -m)}"

case "$machine" in
  x86_64 | amd64)
    asset=amd64
    digest_key=SHFMT_SHA256_LINUX_AMD64
    ;;
  aarch64 | arm64)
    asset=arm64
    digest_key=SHFMT_SHA256_LINUX_ARM64
    ;;
  armv6l | armv7l)
    asset=arm
    digest_key=SHFMT_SHA256_LINUX_ARM
    ;;
  i386 | i686)
    asset=386
    digest_key=SHFMT_SHA256_LINUX_386
    ;;
  *)
    version="$(read_pin SHFMT_VERSION)"
    echo "install-shfmt.sh: no pinned shfmt asset for architecture ${machine}; use" \
      "go install mvdan.cc/sh/v3/cmd/shfmt@${version}" >&2
    exit 2
    ;;
esac

version="$(read_pin SHFMT_VERSION)"
digest="$(read_pin "$digest_key")"
url="https://github.com/mvdan/sh/releases/download/${version}/shfmt_${version}_linux_${asset}"

if [[ "$mode" == resolve ]]; then
  printf '%s %s\n' "$asset" "$digest"
  exit 0
fi

if command -v shfmt >/dev/null; then
  exit 0
fi

if [[ $EUID -eq 0 ]]; then
  SUDO=""
else
  SUDO="sudo"
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
curl --retry 3 --retry-all-errors -fsSL -o "$tmp" "$url"
actual="$(sha256sum "$tmp" | awk '{print $1}')"
if [[ "$actual" != "$digest" ]]; then
  echo "install-shfmt.sh: shfmt ${version} digest mismatch: got ${actual}, want ${digest}" >&2
  exit 1
fi
$SUDO install -m 0755 "$tmp" /usr/local/bin/shfmt
