#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
# Install clang, lldb, lld, clangd and clang-tidy. Prefer the distribution
# archive; ratchet consumers select --source=apt.llvm.org for their baseline.
# llvm.sh is content-pinned: upstream changes require review and a new digest.
# Run as root: install-llvm.sh <llvm-major> [--source=archive-first|--source=apt.llvm.org]
set -euo pipefail

major=${1:-}
source_mode=${2:---source=archive-first}
case $major in
  '' | *[!0-9]*)
    echo "usage: install-llvm.sh <llvm-major> [--source=archive-first|--source=apt.llvm.org]" >&2
    exit 2
    ;;
esac
case $source_mode in
  --source=archive-first | --source=apt.llvm.org) ;;
  *)
    echo "install-llvm: unknown source: $source_mode" >&2
    exit 2
    ;;
esac
if (($# > 2)); then
  echo "install-llvm: too many arguments" >&2
  exit 2
fi
packages=("clang-$major" "lldb-$major" "lld-$major" "clangd-$major" "clang-tidy-$major")

if [[ $source_mode == --source=archive-first ]]; then
  apt-get --error-on=any -o Acquire::Retries=3 update -q
  archive_available=1
  for package in "${packages[@]}"; do
    if ! apt-cache show "$package" >/dev/null 2>&1; then
      echo "install-llvm: distribution archive has no $package" >&2
      archive_available=0
    fi
  done
  if ((archive_available)); then
    if apt-get install -y -q "${packages[@]}"; then
      echo "install-llvm: LLVM $major from the distribution archive"
      exit 0
    fi
    echo "install-llvm: distribution archive install failed; trying apt.llvm.org" >&2
  fi
fi

echo "install-llvm: LLVM $major from apt.llvm.org"
# Reviewed https://apt.llvm.org/llvm.sh, 2026-11-03. License: Apache-2.0 WITH LLVM-exception.
LLVM_INSTALLER_SHA256=03878e08f47b66cc95bc4b544b0db3c6d9ce8d60e6cf2492ae357984330a9eae
tmp_files=()
_cleanup() {
  rm -f "${tmp_files[@]}"
}
trap _cleanup EXIT
# Signal exits run the EXIT cleanup and never resume an interrupted install.
trap 'exit 130' INT
trap 'exit 143' TERM
installer=$(mktemp)
tmp_files+=("$installer")
wget -qO "$installer" --tries=5 --retry-connrefused --retry-on-host-error --waitretry=5 --timeout=30 \
  https://apt.llvm.org/llvm.sh
if ! printf '%s  %s\n' "$LLVM_INSTALLER_SHA256" "$installer" | sha256sum --check --status; then
  echo "install-llvm: llvm.sh SHA-256 mismatch; refusing to execute" >&2
  exit 1
fi
bash "$installer" "$major"
# llvm.sh's default package set omits clang-tidy.
apt-get install -y -q "${packages[@]}"
