#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# macOS (Intel + Apple Silicon). Uses Homebrew. No CUDA (NVIDIA dropped macOS).
# SYCL is possible via Intel oneAPI on Intel Macs only — not supported on M1/M2/M3.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"

ENABLE_SYCL="${ENABLE_SYCL:-false}"
INSTALL_LINTERS="${INSTALL_LINTERS:-true}"

if ! command -v brew >/dev/null; then
  echo "Homebrew not found. Install from https://brew.sh first."
  exit 2
fi

echo "=== macOS setup for vmaf fork ==="
brew update

# Core build tools.
brew install \
  meson ninja nasm pkg-config \
  llvm cppcheck \
  python@3.12 \
  git doxygen ffmpeg

# llvm provides clang-tidy/clang-format not shipped with Apple's clang.
CLANG_BIN="$(brew --prefix llvm)/bin"
echo "Add to PATH:  export PATH=$CLANG_BIN:\$PATH"

if [[ "$INSTALL_LINTERS" == "true" ]]; then
  brew install shellcheck shfmt
  python3 -m pip install --user --require-hashes \
    -r "$REPO_ROOT/requirements/locks/dev-linters.txt"
fi

if [[ "$ENABLE_SYCL" == "true" ]]; then
  ARCH=$(uname -m)
  if [[ "$ARCH" != "x86_64" ]]; then
    echo "SYCL is not supported on Apple Silicon ($ARCH). Build CPU-only."
    exit 2
  fi
  echo "oneAPI on macOS (Intel): download from"
  echo "  https://www.intel.com/content/www/us/en/developer/tools/oneapi/base-toolkit-download.html"
  echo "Then: source /opt/intel/oneapi/setvars.sh"
fi

echo ""
echo "=== done. next steps ==="
echo "  export PATH=$CLANG_BIN:\$PATH"
echo "  meson setup build -Denable_cuda=false -Denable_sycl=$ENABLE_SYCL"
