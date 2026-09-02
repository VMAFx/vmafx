#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Step 01: Prerequisite check for the ensemble training kit (ADR-0324).
#
# Verifies that the host has the toolchain pieces required to run the
# Phase-A corpus generation + LOSO retrain end-to-end. Each check is
# fatal — if anything is missing, the script prints a remediation hint
# and exits non-zero so the operator sees one problem at a time.
#
# Environment overrides:
#   LIBVMAF_BIN  Path to libvmaf-CUDA binary (default: core/build-cuda/tools/vmaf)
#   GPU_MIN_MIB  Minimum free GPU memory in MiB (default: 6144)

set -euo pipefail

KIT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$KIT_DIR/../.." && pwd)}"

# shellcheck source=tools/ensemble-training-kit/_platform_detect.sh
# shellcheck disable=SC1091
source "$KIT_DIR/_platform_detect.sh"
PLATFORM="$(detect_platform)"

LIBVMAF_BIN="${LIBVMAF_BIN:-$REPO_ROOT/core/build-cuda/tools/vmaf}"
GPU_MIN_MIB="${GPU_MIN_MIB:-6144}"

echo "[prereqs] platform=$PLATFORM"
echo "[prereqs] repo_root=$REPO_ROOT"
echo "[prereqs] libvmaf_bin=$LIBVMAF_BIN"
echo

# Inform-only: tell the operator which kit-bundled binary subdir applies
# to this host. Missing binaries on _other_ platforms are fine — the
# operator only runs the kit on one box at a time. Operators who set
# LIBVMAF_BIN explicitly bypass this.
expected_kit_bin="$KIT_DIR/binaries/$PLATFORM/vmaf"
if [[ -x "$expected_kit_bin" ]]; then
  echo "[prereqs] kit-bundled binary present: $expected_kit_bin"
else
  echo "[prereqs] info: kit-bundled binary for $PLATFORM not present at $expected_kit_bin"
  echo "[prereqs] info: build it once via: bash $KIT_DIR/build-libvmaf-binaries.sh --platform $PLATFORM"
fi

# Darwin gate: skip the NVIDIA / CUDA-specific checks below; macOS uses
# the CPU path (NEON / AVX2). Probe ffmpeg's VideoToolbox availability
# instead and stop here.
if [[ "$PLATFORM" == darwin-* ]]; then
  if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "[prereqs] FAIL: ffmpeg not in PATH" >&2
    echo "[prereqs] hint: brew install ffmpeg" >&2
    exit 2
  fi
  if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_videotoolbox; then
    echo "[prereqs] ok: ffmpeg with h264_videotoolbox / hevc_videotoolbox available"
  else
    echo "[prereqs] FAIL: ffmpeg lacks h264_videotoolbox encoder" >&2
    echo "[prereqs] hint: brew install ffmpeg (the brew formula links VideoToolbox by default)" >&2
    exit 2
  fi
  echo "[prereqs] info: NVIDIA / CUDA / iHD checks skipped on Darwin"
  echo
  echo "[prereqs] all checks passed (Darwin VT path)"
  exit 0
fi

fail() {
  echo "[prereqs] FAIL: $1" >&2
  [[ -n "${2:-}" ]] && echo "[prereqs] hint: $2" >&2
  exit 2
}

ok() { echo "[prereqs] ok: $1"; }

# 1. ffmpeg + per-platform encoder probe -------------------------------------
command -v ffmpeg >/dev/null 2>&1 || fail "ffmpeg not in PATH" \
  "install ffmpeg (Linux distros usually ship it; verify via 'ffmpeg -encoders')"
ffmpeg_ver=$(ffmpeg -version 2>/dev/null | head -1)
case "$PLATFORM" in
  linux-x86_64-cuda)
    if ! ffmpeg -hide_banner -encoders 2>&1 | grep -q h264_nvenc; then
      fail "ffmpeg present but lacks h264_nvenc encoder" \
        "rebuild ffmpeg with --enable-libnvenc (NVIDIA Video Codec SDK headers must be on the include path)"
    fi
    ok "ffmpeg + h264_nvenc ($ffmpeg_ver)"
    ;;
  linux-x86_64-sycl)
    if ! ffmpeg -hide_banner -encoders 2>&1 | grep -q h264_qsv; then
      fail "ffmpeg present but lacks h264_qsv encoder" \
        "rebuild ffmpeg with --enable-libmfx (or VPL on newer drivers) and Intel iHD VA-API runtime"
    fi
    ok "ffmpeg + h264_qsv ($ffmpeg_ver)"
    ;;
  linux-x86_64-vulkan)
    ok "ffmpeg ($ffmpeg_ver) — Vulkan/CPU fallback path"
    ;;
esac

# 2. libvmaf binary -----------------------------------------------------------
if [[ ! -x "$LIBVMAF_BIN" ]]; then
  fail "libvmaf binary not found at $LIBVMAF_BIN" \
    "build with: bash $KIT_DIR/build-libvmaf-binaries.sh --platform $PLATFORM (or set LIBVMAF_BIN explicitly)"
fi
# CUDA platform requires a CUDA-enabled binary; other platforms only
# require an executable libvmaf — backend probe is informative.
if [[ "$PLATFORM" == "linux-x86_64-cuda" ]]; then
  if ! "$LIBVMAF_BIN" --help 2>&1 | grep -qi cuda; then
    fail "libvmaf binary at $LIBVMAF_BIN does not advertise CUDA in --help" \
      "rebuild with -Denable_cuda=true; the CPU-only binary will not exercise the corpus generator's --gpumask path"
  fi
  ok "libvmaf-CUDA binary ($LIBVMAF_BIN)"
else
  ok "libvmaf binary ($LIBVMAF_BIN) — backend probe skipped on $PLATFORM"
fi

# 3. Python + torch.cuda ------------------------------------------------------
if ! command -v python3 >/dev/null 2>&1; then
  fail "python3 not in PATH" "install Python >= 3.12"
fi
py_ver=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
py_major=$(python3 -c 'import sys; print(sys.version_info[0])')
py_minor=$(python3 -c 'import sys; print(sys.version_info[1])')
if [[ "$py_major" -lt 3 || ("$py_major" -eq 3 && "$py_minor" -lt 12) ]]; then
  fail "python $py_ver is too old (need >= 3.12)" \
    "install python3.12+; pyenv or your distro's python3.12 package both work"
fi
ok "python $py_ver"

if ! python3 -c "import torch" 2>/dev/null; then
  fail "torch not importable in python3" \
    "pip install -r tools/ensemble-training-kit/requirements-frozen.txt"
fi
torch_ver=$(python3 -c 'import torch; print(torch.__version__)')
if [[ "$PLATFORM" == "linux-x86_64-cuda" ]]; then
  if ! python3 -c "import torch; assert torch.cuda.is_available(), 'no CUDA'" 2>/dev/null; then
    fail "torch.cuda.is_available() is False" \
      "ensure NVIDIA driver + CUDA runtime are installed and torch was built with CUDA support"
  fi
  ok "torch $torch_ver with CUDA"
else
  ok "torch $torch_ver (CUDA probe skipped on $PLATFORM — trainer uses CPU)"
fi

# 4. GPU free memory (CUDA platform only) ------------------------------------
if [[ "$PLATFORM" == "linux-x86_64-cuda" ]]; then
  if ! command -v nvidia-smi >/dev/null 2>&1; then
    fail "nvidia-smi not in PATH" "install the NVIDIA driver"
  fi
  gpu_free_mib=$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits 2>/dev/null |
    head -1 | tr -d '[:space:]')
  if [[ -z "$gpu_free_mib" ]]; then
    fail "nvidia-smi did not report memory.free" "verify the NVIDIA driver is loaded"
  fi
  if [[ "$gpu_free_mib" -lt "$GPU_MIN_MIB" ]]; then
    fail "GPU has only ${gpu_free_mib} MiB free (need >= ${GPU_MIN_MIB})" \
      "free up VRAM; close other CUDA workloads"
  fi
  ok "GPU free memory: ${gpu_free_mib} MiB (>= ${GPU_MIN_MIB})"
fi

# 5. Optional: vainfo for QSV detection (informational) ----------------------
if command -v vainfo >/dev/null 2>&1 && vainfo --display drm 2>/dev/null | grep -q iHD; then
  echo "[prereqs] info: Intel iHD VA-API driver detected (QSV lanes available)"
else
  echo "[prereqs] info: Intel iHD VA-API driver not detected (QSV lanes will be skipped — NVENC-only run)"
fi

echo
echo "[prereqs] all checks passed"
