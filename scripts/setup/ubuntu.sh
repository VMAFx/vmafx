#!/usr/bin/env bash
# Ubuntu 26.04 LTS (Plucky) — also works on 24.04 LTS, 22.04 LTS, Debian 12, Mint 21.
# Installs the build toolchain, Python dev deps, and (optionally) CUDA / oneAPI.
set -euo pipefail

ENABLE_CUDA="${ENABLE_CUDA:-false}"
ENABLE_SYCL="${ENABLE_SYCL:-false}"
INSTALL_LINTERS="${INSTALL_LINTERS:-true}"

need_sudo() {
  if [[ $EUID -ne 0 ]]; then echo "sudo"; else echo ""; fi
}
SUDO="$(need_sudo)"

echo "=== Ubuntu/Debian setup for vmaf fork ==="
$SUDO apt-get update
$SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  build-essential clang clang-tidy clang-format cppcheck \
  meson ninja-build nasm pkg-config xxd \
  python3 python3-pip python3-venv \
  git curl wget ca-certificates gnupg2 \
  doxygen \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavfilter-dev

if [[ "$INSTALL_LINTERS" == "true" ]]; then
  $SUDO apt-get install -y --no-install-recommends shellcheck
  # shfmt from release (not in apt for older Ubuntu).
  # Pin to an explicit version URL — the /releases/latest/download/ route
  # 404s as soon as upstream tags a newer version because the filename
  # contains the version string.
  if ! command -v shfmt >/dev/null; then
    SHFMT_VERSION="v3.13.1"
    $SUDO curl -fsSL -o /usr/local/bin/shfmt \
      "https://github.com/mvdan/sh/releases/download/${SHFMT_VERSION}/shfmt_${SHFMT_VERSION}_linux_amd64"
    $SUDO chmod +x /usr/local/bin/shfmt
  fi
  # Python linters in user-site (no sudo pip).
  python3 -m pip install --user --upgrade \
    pre-commit ruff black isort mypy semgrep
fi

if [[ "$ENABLE_CUDA" == "true" ]]; then
  echo "--- CUDA toolkit (nvidia-cuda-toolkit via apt) ---"
  $SUDO apt-get install -y --no-install-recommends nvidia-cuda-dev nvidia-cuda-toolkit
  echo "Note: for modern CUDA, install from https://developer.nvidia.com/cuda-downloads"
fi

if [[ "$ENABLE_SYCL" == "true" ]]; then
  echo "--- Intel oneAPI DPC++ (SYCL) ---"
  wget -qO- https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB |
    $SUDO gpg --dearmor -o /usr/share/keyrings/oneapi-archive-keyring.gpg
  echo "deb [signed-by=/usr/share/keyrings/oneapi-archive-keyring.gpg] https://apt.repos.intel.com/oneapi all main" |
    $SUDO tee /etc/apt/sources.list.d/oneapi.list >/dev/null
  $SUDO apt-get update
  $SUDO apt-get install -y --no-install-recommends \
    intel-oneapi-compiler-dpcpp-cpp intel-oneapi-runtime-libs \
    libva-dev libva-drm2 level-zero-dev
  echo "After this script, run:  source /opt/intel/oneapi/setvars.sh"
fi

echo ""
echo "=== done. next steps ==="
echo "  meson setup build -Denable_cuda=$ENABLE_CUDA -Denable_sycl=$ENABLE_SYCL"
echo "  meson compile -C build"
echo "  meson test    -C build"
