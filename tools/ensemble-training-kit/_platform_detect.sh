#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# Shared platform-detection helper sourced by 01-prereqs.sh,
# 02-generate-corpus.sh, and run-full-pipeline.sh. Defines two
# functions and exports nothing else into the caller's namespace.
#
#   detect_platform            -> echoes one of:
#       linux-x86_64-cuda
#       linux-x86_64-sycl
#       linux-x86_64-vulkan
#       darwin-arm64-cpu
#       darwin-x86_64-cpu
#       unknown
#
#   detect_default_encoders    -> echoes a comma-separated encoder list
#                                 the platform supports out of the box.
#
# Both functions honour env-var hooks so the test harness can mock
# uname / nvidia-smi / vainfo without poking the real system:
#
#   KIT_FAKE_UNAME_S          override `uname -s`
#   KIT_FAKE_UNAME_M          override `uname -m`
#   KIT_FAKE_HAS_NVIDIA_SMI   "1" / "0" — gate nvidia-smi probe
#   KIT_FAKE_HAS_IHD          "1" / "0" — gate iHD/QSV probe
#   KIT_FAKE_HAS_VIDEOTOOLBOX "1" / "0" — gate ffmpeg VT encoder probe
#
# Without any KIT_FAKE_* override the helper probes the real host.
#
# This file is SOURCED, not executed. We deliberately do NOT enable
# `set -euo pipefail` at the top level because that would mutate the
# caller's shell options. Callers (01-prereqs.sh, 02-generate-corpus.sh,
# run-full-pipeline.sh, tests/test_platform_detect.sh) each set their own
# strict mode and the functions defined here inherit it at call time.

# shellcheck disable=SC2120  # all functions read env vars, no positional args

_kit_uname_s() {
  if [[ -n "${KIT_FAKE_UNAME_S:-}" ]]; then
    printf '%s\n' "$KIT_FAKE_UNAME_S"
  else
    uname -s 2>/dev/null || echo unknown
  fi
}

_kit_uname_m() {
  if [[ -n "${KIT_FAKE_UNAME_M:-}" ]]; then
    printf '%s\n' "$KIT_FAKE_UNAME_M"
  else
    uname -m 2>/dev/null || echo unknown
  fi
}

_kit_has_nvidia() {
  if [[ -n "${KIT_FAKE_HAS_NVIDIA_SMI:-}" ]]; then
    [[ "$KIT_FAKE_HAS_NVIDIA_SMI" == "1" ]]
    return $?
  fi
  command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi >/dev/null 2>&1
}

_kit_has_ihd() {
  if [[ -n "${KIT_FAKE_HAS_IHD:-}" ]]; then
    [[ "$KIT_FAKE_HAS_IHD" == "1" ]]
    return $?
  fi
  command -v vainfo >/dev/null 2>&1 && vainfo --display drm 2>&1 | grep -q iHD
}

_kit_has_videotoolbox() {
  if [[ -n "${KIT_FAKE_HAS_VIDEOTOOLBOX:-}" ]]; then
    [[ "$KIT_FAKE_HAS_VIDEOTOOLBOX" == "1" ]]
    return $?
  fi
  command -v ffmpeg >/dev/null 2>&1 &&
    ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_videotoolbox
}

detect_platform() {
  local s m
  s="$(_kit_uname_s)"
  m="$(_kit_uname_m)"
  case "$s" in
    Linux)
      if _kit_has_nvidia; then
        echo "linux-x86_64-cuda"
      elif _kit_has_ihd; then
        echo "linux-x86_64-sycl"
      else
        echo "linux-x86_64-vulkan"
      fi
      ;;
    Darwin)
      case "$m" in
        arm64) echo "darwin-arm64-cpu" ;;
        x86_64) echo "darwin-x86_64-cpu" ;;
        *) echo "darwin-arm64-cpu" ;;
      esac
      ;;
    *)
      echo "unknown"
      ;;
  esac
}

detect_default_encoders() {
  local s
  s="$(_kit_uname_s)"
  local out=()
  case "$s" in
    Linux)
      if _kit_has_nvidia; then
        out+=("h264_nvenc" "hevc_nvenc" "av1_nvenc")
      fi
      if _kit_has_ihd; then
        out+=("h264_qsv" "hevc_qsv" "av1_qsv")
      fi
      if [[ "${#out[@]}" -eq 0 ]]; then
        # Vulkan-only or unrecognised Linux — fall back to libx264 CPU.
        out+=("libx264")
      fi
      ;;
    Darwin)
      if _kit_has_videotoolbox; then
        out+=("h264_videotoolbox" "hevc_videotoolbox")
      else
        out+=("libx264")
      fi
      ;;
    *)
      out+=("libx264")
      ;;
  esac
  local IFS=','
  echo "${out[*]}"
}
