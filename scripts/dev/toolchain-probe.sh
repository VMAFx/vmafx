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
#   - MinGW cross: x86_64-w64-mingw32-gcc — thread model, runtime DLL
#     resolution and the objdump pairing check (Linux -> Windows)
#
# Emits fenced-block friendly lines:
#   toolchain-probe v1 host=<uname -n> os=<uname -s> date=<date -u +%FT%TZ>
#   toolchain-probe: <name> found <path> <first version line>
#   toolchain-probe: <name> absent searched=<dirs>
#   toolchain-probe: <name> dumpversion <gcc -dumpversion>
#   toolchain-probe: <name> thread-model <model>
#   toolchain-probe: <name> runtime <lib>=<resolved path>
#   toolchain-probe: <name> pairing ok|pairing-mismatch|pairing-unverified ...
#
# Exit codes:
#   0 — probe executed successfully (toolchains may be found or absent).
#   1 — MinGW cross runtime failure (issue #3307): a found cross compiler's
#       runtime DLLs did not resolve via -print-file-name, or the flavour of
#       the runtime contradicts the compiler's thread model. A host without
#       the cross toolchain at all stays exit 0: absence is a legitimate
#       finding, a wrong pairing is not.
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

Probes host compilers (icpx, icx, nvcc, hipcc, glslc, glslangValidator, metal)
and the MinGW cross driver (x86_64-w64-mingw32-gcc: thread model, runtime DLL
pairing via -print-file-name and objdump). MINGW_CROSS_PREFIX overrides where
the cross driver is looked up.
Exit code 0 on normal completion, 1 on a MinGW cross runtime resolution or
thread-model pairing failure, 2 on usage error.
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
  while IFS= read -r sv || [ -n "$sv" ]; do
    candidates+=("$sv")
  done < <(oneapi_setvars_candidates)
  local -i idx=0
  if [ "${#candidates[@]}" -gt 0 ]; then
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
  fi
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

# ---------------------------------------------------------------------------
# MinGW cross entry (Linux -> Windows). Presence is not pairing: the apt
# mingw-w64 gcc-13 driver on ubuntu-24.04 uses the win32 thread model, and a
# posix-flavoured libstdc++-6.dll staged next to it made every version line
# look correct while every real scoring run deadlocked (issue #3307).
# ---------------------------------------------------------------------------

# Prints the first "Thread model:" line of the driver's -v output (win32,
# posix, single); prints nothing when the driver reports none. -v writes to
# stderr, so both streams are read, and CR is stripped for hosts whose gcc
# prints CRLF.
mingw_thread_model() {
  local driver="$1" out="" rc=0 line=""
  out="$("$driver" -v 2>&1)" || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "toolchain-probe: note ${driver} -v exited ${rc}" >&2
  fi
  while IFS= read -r line; do
    line="${line%$'\r'}"
    if [[ "${line}" == "Thread model: "* ]]; then
      printf '%s\n' "${line#"Thread model: "}"
      return 0
    fi
  done <<<"$out"
  return 0
}

# Resolves a runtime DLL the way the stage must: the driver is asked where
# its own runtime lives. -print-file-name answers with an absolute path to
# the file the driver actually links, or echoes the bare argument back when
# it cannot resolve it, so the absolute form plus existence is the success
# signal. A version string is never parsed into a path: Debian's flavoured
# drivers report -dumpversion as "13-win32" and a path rebuilt from it
# silently stages nothing. Prints the path; returns 1 when unresolved.
mingw_resolve_runtime() {
  local driver="$1" lib="$2" p="" rc=0
  p="$("$driver" -print-file-name="${lib}" 2>/dev/null)" || rc=$?
  if [ "$rc" -ne 0 ]; then
    return 1
  fi
  p="$(printf '%s' "${p}" | tr -d '\r')"
  case "${p}" in
    /* | [A-Za-z]:/*)
      if [ -f "${p}" ]; then
        printf '%s\n' "${p}"
        return 0
      fi
      ;;
  esac
  return 1
}

# Asserts the runtime flavour against the compiler's thread model by reading
# the PE import table (objdump -p) of the resolved libstdc++-6.dll — the B24
# regression: a win32-model driver whose libstdc++-6.dll imports libwinpthread
# has a posix runtime staged next to the wrong compiler, and every pthread
# use deadlocks on the target. win32 requires no libwinpthread import, posix
# requires it, and an undetected model refuses: unknown is not evidence.
mingw_assert_pairing() {
  local driver="$1" objdump_bin="$2" model="$3" dll="$4" dump="" rc=0 imports=""
  dump="$("${objdump_bin}" -p "${dll}" 2>&1)" || rc=$?
  if [ "$rc" -ne 0 ]; then
    printf "toolchain-probe: %s pairing-unverified %s: objdump -p exited %d\n" \
      "${driver}" "$(basename "${dll}")" "${rc}"
    return 1
  fi
  if printf '%s\n' "${dump}" | grep -q "libwinpthread"; then
    imports="libwinpthread"
  else
    imports="none"
  fi
  case "${model}" in
    win32)
      if [ "${imports}" = "libwinpthread" ]; then
        printf "toolchain-probe: %s pairing-mismatch thread-model win32 but %s imports libwinpthread (B24: a posix runtime next to a win32-model driver deadlocks on the target)\n" \
          "${driver}" "$(basename "${dll}")"
        return 1
      fi
      printf "toolchain-probe: %s pairing ok thread-model win32, %s imports no libwinpthread\n" \
        "${driver}" "$(basename "${dll}")"
      ;;
    posix)
      if [ "${imports}" = "none" ]; then
        printf "toolchain-probe: %s pairing-mismatch thread-model posix but %s imports no libwinpthread (B24 inverse: a win32 runtime next to a posix-model driver)\n" \
          "${driver}" "$(basename "${dll}")"
        return 1
      fi
      printf "toolchain-probe: %s pairing ok thread-model posix, %s imports libwinpthread\n" \
        "${driver}" "$(basename "${dll}")"
      ;;
    *)
      printf "toolchain-probe: %s pairing-unverified thread-model unknown (%s imports %s); refusing instead of guessing a flavour\n" \
        "${driver}" "$(basename "${dll}")" "${imports}"
      return 1
      ;;
  esac
  return 0
}

# The MinGW cross probe. Exits non-zero only for a FOUND driver whose runtime
# cannot be resolved or whose flavour contradicts the thread model; the
# absence of the cross toolchain on a host is exit 0, like the other tools.
probe_mingw_cross() {
  local driver="$1" libgcc="$2"
  local p="" searched="PATH" model="" stdcxx="" rp="" objdump_bin="" lib="" fail=0
  if [ -n "${MINGW_CROSS_PREFIX:-}" ]; then
    if [ -x "${MINGW_CROSS_PREFIX}/bin/${driver}" ]; then
      p="${MINGW_CROSS_PREFIX}/bin/${driver}"
    fi
    searched="${MINGW_CROSS_PREFIX}/bin"
  elif ! p="$(command -v "${driver}")"; then
    p=""
  fi
  if [ -z "${p}" ]; then
    printf "toolchain-probe: %s absent searched=%s\n" "${driver}" "${searched}"
    return 0
  fi

  printf "toolchain-probe: %s found %s %s\n" \
    "${driver}" "${p}" "$(get_cmd_version "${p}" --version)"
  printf "toolchain-probe: %s dumpversion %s\n" \
    "${driver}" "$(get_cmd_version "${p}" -dumpversion)"
  model="$(mingw_thread_model "${p}")"
  printf "toolchain-probe: %s thread-model %s\n" "${driver}" "${model:-unknown}"

  # Required runtime: the stage is only complete when both DLLs resolve.
  # Every resolution failure is reported before the entry refuses, so one
  # pasted block carries the whole evidence.
  for lib in "libstdc++-6.dll" "${libgcc}"; do
    if rp="$(mingw_resolve_runtime "${p}" "${lib}")"; then
      printf "toolchain-probe: %s runtime %s=%s\n" "${driver}" "${lib}" "${rp}"
      if [ "${lib}" = "libstdc++-6.dll" ]; then
        stdcxx="${rp}"
      fi
    else
      printf "toolchain-probe: %s runtime-unresolved %s (gcc -print-file-name answered no absolute path; refusing instead of staging green)\n" \
        "${driver}" "${lib}"
      fail=1
    fi
  done
  if [ "${fail}" -ne 0 ]; then
    return 1
  fi

  # Evidence only: not every flavour ships libwinpthread, so its resolution
  # never fails the entry.
  if rp="$(mingw_resolve_runtime "${p}" "libwinpthread-1.dll")"; then
    printf "toolchain-probe: %s runtime libwinpthread-1.dll=%s\n" "${driver}" "${rp}"
  else
    printf "toolchain-probe: %s runtime libwinpthread-1.dll unresolved (evidence only; not every flavour ships it)\n" "${driver}"
  fi

  # objdump: the target-prefixed binary next to the driver first, then PATH.
  objdump_bin="$(dirname "${p}")/${driver%-gcc}-objdump"
  if [ ! -x "${objdump_bin}" ]; then
    if ! objdump_bin="$(command -v "${driver%-gcc}-objdump")"; then
      if ! objdump_bin="$(command -v objdump)"; then
        printf "toolchain-probe: %s pairing-unverified no objdump found (target-prefixed or plain); refusing to pass an unasserted stage\n" "${driver}"
        return 1
      fi
    fi
  fi

  mingw_assert_pairing "${driver}" "${objdump_bin}" "${model:-unknown}" "${stdcxx}"
}

# Run probes in bounded order
probe_oneapi_tool "icpx"
probe_oneapi_tool "icx"
probe_nvcc
probe_hipcc
probe_vulkan_tool "glslc"
probe_vulkan_tool "glslangValidator"
probe_metal

# MinGW cross entry last: it is the only entry that can fail the probe (a
# found cross compiler whose runtime resolution or thread-model pairing
# fails — issue #3307), and its verdict must not be masked by reordering.
mingw_status=0
probe_mingw_cross "x86_64-w64-mingw32-gcc" "libgcc_s_seh-1.dll" || mingw_status=$?

exit "${mingw_status}"
