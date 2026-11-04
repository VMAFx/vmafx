#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/dev/toolchain-probe.sh — probe host build and vendor compiler toolchains.
#
# Probes toolchains on PATH and in standard off-PATH vendor locations:
#   - oneAPI: icpx, icx via setvars.sh
#   - CUDA: nvcc
#   - ROCm: hipcc
#   - Vulkan: glslc, glslangValidator
#   - Metal: xcrun metal (macOS only)
#
# Emits fenced-block friendly lines:
#   toolchain-probe v1 host=<uname -n> os=<uname -s> date=<date -u +%FT%TZ>
#   toolchain-probe: <name> found <path> <first version line>
#   toolchain-probe: <name> absent searched=<dirs>
#
# Exit codes:
#   0 — probe executed successfully (toolchains may be found or absent).
#   2 — usage error (invalid command-line arguments).

set -euo pipefail

oneapi_prefix_arg=""

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix | --oneapi-prefix)
      if [ $# -lt 2 ]; then
        echo "toolchain-probe: $1 requires a path argument" >&2
        exit 2
      fi
      oneapi_prefix_arg="$2"
      shift 2
      ;;
    -h | --help)
      cat <<'EOF'
Usage: scripts/dev/toolchain-probe.sh [--prefix PATH]

Probes host compilers (icpx, icx, nvcc, hipcc, glslc, glslangValidator, metal).
Exit code 0 on normal completion, 2 on usage error.
EOF
      exit 0
      ;;
    *)
      echo "toolchain-probe: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

ONEAPI_PREFIX="${oneapi_prefix_arg:-${ONEAPI_PREFIX:-}}"

# First line: fenced-block header
host_name="$(uname -n)"
os_name="$(uname -s)"
now_utc="$(date -u +%FT%TZ)"
printf "toolchain-probe v1 host=%s os=%s date=%s\n" "$host_name" "$os_name" "$now_utc"

# Safe first-line extractor avoiding SIGPIPE under set -e / pipefail
get_cmd_version() {
  local output="" line=""
  local -i rc=0
  # A version query may exit non-zero and still print the version; the
  # status is kept and printed when no output line arrives.
  output="$("$@" 2>&1)" || rc=$?
  local -i line_count=0
  while IFS= read -r line; do
    line_count=$((line_count + 1))
    if [ "$line_count" -gt 50 ]; then break; fi
    if [[ "$line" =~ [^[:space:]] ]]; then
      printf "%s" "$line" | tr -d '\r'
      return 0
    fi
  done <<<"$output"
  printf "unknown(exit=%d)" "$rc"
}

# Prints the oneAPI setvars.sh candidates, one per line: ONEAPI_PREFIX alone
# when set (tests point it at a stub), else ONEAPI_ROOT, then the unversioned
# default install, then versioned side installs (oneapi-2025.3 and the like).
oneapi_setvars_candidates() {
  local -a roots=()
  if [ -n "${ONEAPI_PREFIX:-}" ]; then
    roots=("${ONEAPI_PREFIX}")
  else
    if [ -n "${ONEAPI_ROOT:-}" ]; then
      roots+=("${ONEAPI_ROOT}")
    fi
    roots+=(/opt/intel/oneapi /opt/intel/oneapi-*)
  fi
  local root
  local -i idx=0
  for root in "${roots[@]}"; do
    idx=$((idx + 1))
    if [ "$idx" -gt 50 ]; then break; fi
    if [ -f "${root}/setvars.sh" ]; then
      printf '%s\n' "${root}/setvars.sh"
    elif [ -f "${root}" ]; then
      printf '%s\n' "${root}"
    fi
  done
}

# Sources one setvars.sh in a subshell and prints the tool path it puts on
# PATH; returns 1 when the tool stays absent. setvars.sh exits non-zero on
# partial installs that still set PATH, so its status is reported on stderr
# and the PATH lookup decides.
oneapi_tool_from_setvars() {
  local setvars="$1" tool="$2"
  (
    set +e
    # shellcheck disable=SC1090
    source "$setvars" --force >/dev/null 2>&1
    sv_rc=$?
    if [ "$sv_rc" -ne 0 ]; then
      echo "toolchain-probe: note ${setvars} exited ${sv_rc}" >&2
    fi
    command -v "$tool"
  )
}

probe_oneapi_tool() {
  local tool="$1" p="" sv
  local searched_desc="PATH"
  local -a candidates=()
  if [ -z "${ONEAPI_PREFIX:-}" ] && p="$(command -v "$tool")"; then
    printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$(get_cmd_version "$p" --version)"
    return 0
  fi
  mapfile -t candidates < <(oneapi_setvars_candidates)
  local -i idx=0
  for sv in "${candidates[@]}"; do
    idx=$((idx + 1))
    if [ "$idx" -gt 50 ]; then break; fi
    searched_desc="${searched_desc},$(dirname "$sv")"
    if p="$(oneapi_tool_from_setvars "$sv" "$tool")" && [ -x "$p" ]; then
      printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" \
        "$(get_cmd_version oneapi_run_with_setvars "$sv" "$p" --version)"
      return 0
    fi
  done
  if [ -z "${ONEAPI_PREFIX:-}" ]; then
    local bin
    for bin in /opt/intel/oneapi/compiler/*/bin/"$tool" /opt/intel/oneapi-*/compiler/*/bin/"$tool"; do
      if [ -x "$bin" ]; then
        printf "toolchain-probe: %s found %s %s\n" "$tool" "$bin" "$(get_cmd_version "$bin" --version)"
        return 0
      fi
    done
    searched_desc="${searched_desc},/opt/intel/oneapi*"
  else
    searched_desc="${ONEAPI_PREFIX}"
  fi
  printf "toolchain-probe: %s absent searched=%s\n" "$tool" "$searched_desc"
}

# Runs a command with one setvars.sh sourced (icpx needs its environment to
# print a version).
# shellcheck disable=SC2329 # called through get_cmd_version "$@"
oneapi_run_with_setvars() {
  local setvars="$1"
  shift
  (
    set +e
    # shellcheck disable=SC1090
    source "$setvars" --force >/dev/null 2>&1
    "$@"
  )
}

probe_nvcc() {
  local tool="nvcc"
  if command -v "$tool" >/dev/null 2>&1; then
    local p v
    p="$(command -v "$tool")"
    v="$(get_cmd_version "$p" --version)"
    printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$v"
    return 0
  fi

  if [ -n "${CUDA_PATH:-}" ] && [ -x "${CUDA_PATH}/bin/${tool}" ]; then
    local p v
    p="${CUDA_PATH}/bin/${tool}"
    v="$(get_cmd_version "$p" --version)"
    printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$v"
    return 0
  fi

  local -i c_idx=0
  for bin in /opt/cuda*/bin/"$tool" /usr/local/cuda*/bin/"$tool"; do
    c_idx=$((c_idx + 1))
    if [ "$c_idx" -gt 50 ]; then break; fi
    if [ -x "$bin" ]; then
      local v
      v="$(get_cmd_version "$bin" --version)"
      printf "toolchain-probe: %s found %s %s\n" "$tool" "$bin" "$v"
      return 0
    fi
  done

  local searched="PATH"
  if [ -n "${CUDA_PATH:-}" ]; then
    searched="${searched},${CUDA_PATH}"
  fi
  searched="${searched},/opt/cuda,/usr/local/cuda"
  printf "toolchain-probe: %s absent searched=%s\n" "$tool" "$searched"
}

probe_hipcc() {
  local tool="hipcc"
  if command -v "$tool" >/dev/null 2>&1; then
    local p v
    p="$(command -v "$tool")"
    v="$(get_cmd_version "$p" --version)"
    printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$v"
    return 0
  fi

  if [ -n "${ROCM_PATH:-}" ] && [ -x "${ROCM_PATH}/bin/${tool}" ]; then
    local p v
    p="${ROCM_PATH}/bin/${tool}"
    v="$(get_cmd_version "$p" --version)"
    printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$v"
    return 0
  fi

  local -i r_idx=0
  for bin in /opt/rocm*/bin/"$tool"; do
    r_idx=$((r_idx + 1))
    if [ "$r_idx" -gt 50 ]; then break; fi
    if [ -x "$bin" ]; then
      local v
      v="$(get_cmd_version "$bin" --version)"
      printf "toolchain-probe: %s found %s %s\n" "$tool" "$bin" "$v"
      return 0
    fi
  done

  local searched="PATH"
  if [ -n "${ROCM_PATH:-}" ]; then
    searched="${searched},${ROCM_PATH}"
  fi
  searched="${searched},/opt/rocm"
  printf "toolchain-probe: %s absent searched=%s\n" "$tool" "$searched"
}

probe_vulkan_tool() {
  local tool="$1"
  if command -v "$tool" >/dev/null 2>&1; then
    local p v
    p="$(command -v "$tool")"
    v="$(get_cmd_version "$p" --version)"
    printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$v"
    return 0
  fi

  if [ -n "${VULKAN_SDK:-}" ] && [ -x "${VULKAN_SDK}/bin/${tool}" ]; then
    local p v
    p="${VULKAN_SDK}/bin/${tool}"
    v="$(get_cmd_version "$p" --version)"
    printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$v"
    return 0
  fi

  local searched="PATH"
  if [ -n "${VULKAN_SDK:-}" ]; then
    searched="${searched},${VULKAN_SDK}/bin"
  fi
  printf "toolchain-probe: %s absent searched=%s\n" "$tool" "$searched"
}

probe_metal() {
  if [ "$(uname -s)" != "Darwin" ]; then
    return 0
  fi
  local tool="metal"
  if command -v xcrun >/dev/null 2>&1; then
    if xcrun -sdk macosx metal --version >/dev/null 2>&1; then
      local p v
      p="$(xcrun -sdk macosx --find metal 2>/dev/null || echo "xcrun metal")"
      v="$(get_cmd_version xcrun -sdk macosx metal --version)"
      printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$v"
      return 0
    elif xcrun metal --version >/dev/null 2>&1; then
      local p v
      p="$(xcrun --find metal 2>/dev/null || echo "xcrun metal")"
      v="$(get_cmd_version xcrun metal --version)"
      printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$v"
      return 0
    fi
  elif command -v metal >/dev/null 2>&1; then
    local p v
    p="$(command -v metal)"
    v="$(get_cmd_version "$p" --version)"
    printf "toolchain-probe: %s found %s %s\n" "$tool" "$p" "$v"
    return 0
  fi
  printf "toolchain-probe: %s absent searched=xcrun,PATH\n" "$tool"
}

# Run probes in bounded order
probe_oneapi_tool "icpx"
probe_oneapi_tool "icx"
probe_nvcc
probe_hipcc
probe_vulkan_tool "glslc"
probe_vulkan_tool "glslangValidator"
probe_metal

exit 0
