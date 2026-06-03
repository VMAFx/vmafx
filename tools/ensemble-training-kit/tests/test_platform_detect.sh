#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Smoke tests for tools/ensemble-training-kit/_platform_detect.sh.
# Exercises detect_platform / detect_default_encoders against mocked
# uname / nvidia-smi / vainfo / ffmpeg signals via KIT_FAKE_* env vars.
#
# Run: bash tools/ensemble-training-kit/tests/test_platform_detect.sh
# Exit 0 if all assertions hold; non-zero on first failure.

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/ensemble-training-kit/_platform_detect.sh
# shellcheck disable=SC1091
source "$KIT_DIR/_platform_detect.sh"

failures=0

assert_eq() {
  local got="$1" want="$2" label="$3"
  if [[ "$got" == "$want" ]]; then
    echo "  ok: $label -> $got"
  else
    echo "  FAIL: $label want='$want' got='$got'" >&2
    failures=$((failures + 1))
  fi
}

# Wipe the fake gates between cases so each scenario is self-contained.
reset_fakes() {
  unset KIT_FAKE_UNAME_S KIT_FAKE_UNAME_M
  unset KIT_FAKE_HAS_NVIDIA_SMI KIT_FAKE_HAS_IHD KIT_FAKE_HAS_VIDEOTOOLBOX
}

echo "[test] case 1: NVIDIA Linux box"
reset_fakes
export KIT_FAKE_UNAME_S=Linux KIT_FAKE_UNAME_M=x86_64
export KIT_FAKE_HAS_NVIDIA_SMI=1 KIT_FAKE_HAS_IHD=0 KIT_FAKE_HAS_VIDEOTOOLBOX=0
assert_eq "$(detect_platform)" "linux-x86_64-cuda" "platform"
assert_eq "$(detect_default_encoders)" "h264_nvenc,hevc_nvenc,av1_nvenc" "encoders"

echo "[test] case 2: NVIDIA + Intel iHD coexist (NVIDIA wins for platform tag)"
reset_fakes
export KIT_FAKE_UNAME_S=Linux KIT_FAKE_UNAME_M=x86_64
export KIT_FAKE_HAS_NVIDIA_SMI=1 KIT_FAKE_HAS_IHD=1 KIT_FAKE_HAS_VIDEOTOOLBOX=0
assert_eq "$(detect_platform)" "linux-x86_64-cuda" "platform"
assert_eq "$(detect_default_encoders)" "h264_nvenc,hevc_nvenc,av1_nvenc,h264_qsv,hevc_qsv,av1_qsv" "encoders"

echo "[test] case 3: Intel Arc / iGPU Linux box (iHD only, no NVIDIA)"
reset_fakes
export KIT_FAKE_UNAME_S=Linux KIT_FAKE_UNAME_M=x86_64
export KIT_FAKE_HAS_NVIDIA_SMI=0 KIT_FAKE_HAS_IHD=1 KIT_FAKE_HAS_VIDEOTOOLBOX=0
assert_eq "$(detect_platform)" "linux-x86_64-sycl" "platform"
assert_eq "$(detect_default_encoders)" "h264_qsv,hevc_qsv,av1_qsv" "encoders"

echo "[test] case 4: Vulkan/CPU-only Linux box"
reset_fakes
export KIT_FAKE_UNAME_S=Linux KIT_FAKE_UNAME_M=x86_64
export KIT_FAKE_HAS_NVIDIA_SMI=0 KIT_FAKE_HAS_IHD=0 KIT_FAKE_HAS_VIDEOTOOLBOX=0
assert_eq "$(detect_platform)" "linux-x86_64-vulkan" "platform"
assert_eq "$(detect_default_encoders)" "libx264" "encoders"

echo "[test] case 5: Apple Silicon Mac with VideoToolbox"
reset_fakes
export KIT_FAKE_UNAME_S=Darwin KIT_FAKE_UNAME_M=arm64
export KIT_FAKE_HAS_NVIDIA_SMI=0 KIT_FAKE_HAS_IHD=0 KIT_FAKE_HAS_VIDEOTOOLBOX=1
assert_eq "$(detect_platform)" "darwin-arm64-cpu" "platform"
assert_eq "$(detect_default_encoders)" "h264_videotoolbox,hevc_videotoolbox" "encoders"

echo "[test] case 6: Intel Mac with VideoToolbox"
reset_fakes
export KIT_FAKE_UNAME_S=Darwin KIT_FAKE_UNAME_M=x86_64
export KIT_FAKE_HAS_NVIDIA_SMI=0 KIT_FAKE_HAS_IHD=0 KIT_FAKE_HAS_VIDEOTOOLBOX=1
assert_eq "$(detect_platform)" "darwin-x86_64-cpu" "platform"
assert_eq "$(detect_default_encoders)" "h264_videotoolbox,hevc_videotoolbox" "encoders"

echo "[test] case 7: Mac without ffmpeg-VT falls back to libx264"
reset_fakes
export KIT_FAKE_UNAME_S=Darwin KIT_FAKE_UNAME_M=arm64
export KIT_FAKE_HAS_NVIDIA_SMI=0 KIT_FAKE_HAS_IHD=0 KIT_FAKE_HAS_VIDEOTOOLBOX=0
assert_eq "$(detect_platform)" "darwin-arm64-cpu" "platform"
assert_eq "$(detect_default_encoders)" "libx264" "encoders"

echo "[test] case 8: unknown OS"
reset_fakes
export KIT_FAKE_UNAME_S=FreeBSD KIT_FAKE_UNAME_M=x86_64
export KIT_FAKE_HAS_NVIDIA_SMI=0 KIT_FAKE_HAS_IHD=0 KIT_FAKE_HAS_VIDEOTOOLBOX=0
assert_eq "$(detect_platform)" "unknown" "platform"
assert_eq "$(detect_default_encoders)" "libx264" "encoders"

echo
if [[ "$failures" -eq 0 ]]; then
  echo "[test] all 16 assertions passed"
  exit 0
else
  echo "[test] $failures assertion(s) failed" >&2
  exit 1
fi
