#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Ubuntu 26.04 LTS (Resolute) — also works on 24.04 LTS, 22.04 LTS, Debian 12, Mint 21.
# Installs the build toolchain, Python dev deps, and (optionally) CUDA / oneAPI.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${VMAFX_REPO_ROOT:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}"

ENABLE_CUDA="${ENABLE_CUDA:-false}"
ENABLE_SYCL="${ENABLE_SYCL:-false}"
INSTALL_LINTERS="${INSTALL_LINTERS:-true}"
VMAFX_TOOLS_VENV="${VMAFX_TOOLS_VENV:-${XDG_DATA_HOME:-$HOME/.local/share}/vmafx/tools-venv}"

require_bool_flag() {
  local name="$1"
  local value="$2"
  case "$value" in
    true | false) ;;
    *)
      echo "ubuntu.sh: ${name} must be true or false, got '${value}'" >&2
      exit 2
      ;;
  esac
}

require_bool_flag ENABLE_CUDA "$ENABLE_CUDA"
require_bool_flag ENABLE_SYCL "$ENABLE_SYCL"
require_bool_flag INSTALL_LINTERS "$INSTALL_LINTERS"

required_locks=("$REPO_ROOT/requirements/locks/build.txt")
if [[ "$INSTALL_LINTERS" == "true" ]]; then
  required_locks+=("$REPO_ROOT/requirements/locks/dev-linters.txt")
fi
for lock_file in "${required_locks[@]}"; do
  if [[ ! -f "$lock_file" || ! -r "$lock_file" ]]; then
    echo "ubuntu.sh: lock file ${lock_file} not found; run from a checkout or set VMAFX_REPO_ROOT" >&2
    exit 1
  fi
done

prepare_tools_venv() {
  local venv_dir="$1"
  if [[ -e "$venv_dir" && ! -d "$venv_dir" ]]; then
    echo "ubuntu.sh: VMAFX_TOOLS_VENV '${venv_dir}' exists and is not a directory" >&2
    exit 1
  fi
  if [[ -d "$venv_dir" && ! -f "$venv_dir/pyvenv.cfg" ]]; then
    if [[ -n "$(find "$venv_dir" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
      echo "ubuntu.sh: VMAFX_TOOLS_VENV '${venv_dir}' is non-empty and has no pyvenv.cfg" >&2
      exit 1
    fi
  fi
  python3 -m venv "$venv_dir"
}

need_sudo() {
  if [[ $EUID -ne 0 ]]; then echo "sudo"; else echo ""; fi
}
SUDO="$(need_sudo)"

echo "=== Ubuntu/Debian setup for vmaf fork ==="
$SUDO apt-get --error-on=any -o Acquire::Retries=3 update
# `$SUDO VAR=value cmd` breaks when $SUDO is empty (running as root): bash
# picks out assignments before expanding $SUDO, so VAR=value becomes the
# command name. Pass the variable through `env` instead.
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  build-essential clang clang-tidy clang-format cppcheck \
  ninja-build nasm pkg-config xxd \
  python3 python3-pip python3-venv \
  git curl wget ca-certificates gnupg2 \
  doxygen \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavfilter-dev \
  libzimg-dev
# meson from PyPI, not apt: Ubuntu 24.04 ships meson 1.3.2, which predates
# `c23` in c_std (added in 1.4.0); core/meson.build declares >= 1.4.0 since
# ADR-0692. A dedicated tools venv avoids PEP 668 externally-managed-environment
# errors without sudo pip (ADR-3545).
prepare_tools_venv "$VMAFX_TOOLS_VENV"
"$VMAFX_TOOLS_VENV/bin/python" -m pip install --require-hashes \
  -r "$REPO_ROOT/requirements/locks/build.txt"

if [[ "$INSTALL_LINTERS" == "true" ]]; then
  $SUDO apt-get install -y --no-install-recommends shellcheck
  # shfmt from release (not in apt for older Ubuntu).
  # Pin to an explicit version URL — the /releases/latest/download/ route
  # 404s as soon as upstream tags a newer version because the filename
  # contains the version string.
  if ! command -v shfmt >/dev/null; then
    SHFMT_VERSION="v3.13.1"
    $SUDO curl --retry 3 --retry-all-errors -fsSL -o /usr/local/bin/shfmt \
      "https://github.com/mvdan/sh/releases/download/${SHFMT_VERSION}/shfmt_${SHFMT_VERSION}_linux_amd64"
    $SUDO chmod +x /usr/local/bin/shfmt
  fi
  # Python linters in the tools venv (no sudo pip). isort was retired in
  # ADR-1126; ruff's `I` ruleset is the only import sorter now.
  "$VMAFX_TOOLS_VENV/bin/python" -m pip install --require-hashes \
    -r "$REPO_ROOT/requirements/locks/dev-linters.txt"
fi

if [[ -n "${GITHUB_PATH:-}" ]]; then
  printf '%s\n' "$VMAFX_TOOLS_VENV/bin" >>"$GITHUB_PATH"
fi

if [[ "$ENABLE_CUDA" == "true" ]]; then
  echo "--- CUDA toolkit (nvidia-cuda-toolkit via apt) ---"
  $SUDO apt-get install -y --no-install-recommends nvidia-cuda-dev nvidia-cuda-toolkit
  echo "Note: for modern CUDA, install from https://developer.nvidia.com/cuda-downloads"
fi

if [[ "$ENABLE_SYCL" == "true" ]]; then
  echo "--- Intel oneAPI DPC++ (SYCL) ---"
  key="$(mktemp)"
  wget --tries=3 --retry-connrefused --retry-on-host-error -qO "$key" https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB
  $SUDO gpg --dearmor -o /usr/share/keyrings/oneapi-archive-keyring.gpg "$key"
  rm -f "$key"
  echo "deb [signed-by=/usr/share/keyrings/oneapi-archive-keyring.gpg] https://apt.repos.intel.com/oneapi all main" |
    $SUDO tee /etc/apt/sources.list.d/oneapi.list >/dev/null
  $SUDO apt-get --error-on=any -o Acquire::Retries=3 update
  $SUDO apt-get install -y --no-install-recommends \
    intel-oneapi-compiler-dpcpp-cpp intel-oneapi-runtime-libs \
    libva-dev libva-drm2 level-zero-dev
  echo "After this script, run:  source /opt/intel/oneapi/setvars.sh"
fi

echo ""
echo "=== done. next steps ==="
echo "  export PATH=\"$VMAFX_TOOLS_VENV/bin:\$PATH\""
echo "  meson setup build core -Denable_cuda=$ENABLE_CUDA -Denable_sycl=$ENABLE_SYCL"
echo "  meson compile -C build"
echo "  python3 scripts/ci/run_meson_test.py -- -C build"
