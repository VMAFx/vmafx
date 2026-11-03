#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
# Install an LLVM toolchain major (clang, lldb, lld, clangd), the package set
# of `llvm.sh <major>`. The distribution archive is preferred when it carries
# that major, so CI does not depend on apt.llvm.org being reachable; otherwise
# the apt.llvm.org installer is fetched with bounded retries. The source used
# is printed, never substituted silently. Run as root.
set -euo pipefail

major=${1:-}
case $major in
  '' | *[!0-9]*)
    echo "usage: install-llvm.sh <llvm-major>" >&2
    exit 2
    ;;
esac
packages=("clang-$major" "lldb-$major" "lld-$major" "clangd-$major")

apt-get update -q
if apt-cache show "clang-$major" >/dev/null 2>&1; then
  echo "install-llvm: LLVM $major from the distribution archive"
  apt-get install -y -q "${packages[@]}"
  exit 0
fi

echo "install-llvm: LLVM $major from apt.llvm.org (the distribution has no clang-$major)"
installer=$(mktemp)
trap 'rm -f "$installer"' EXIT
wget -qO "$installer" --tries=5 --retry-connrefused --waitretry=5 --timeout=30 \
  https://apt.llvm.org/llvm.sh
bash "$installer" "$major"
