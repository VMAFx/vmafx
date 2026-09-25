#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Isolated deterministic CPU build profile runner and compiler validator
# for the Netflix golden-data gate (ADR-1317).
#
# Prevents floating-point drift caused by oneAPI ICX or non-deterministic
# compiler optimization settings by enforcing an isolated build directory
# with an explicitly supported compiler (gcc or clang).

set -euo pipefail

validate_compiler_id() {
  local build_dir="$1"
  local info_file="${build_dir}/meson-info/intro-compilers.json"

  if [[ ! -f "${info_file}" ]]; then
    echo "error: build directory '${build_dir}' has no intro-compilers.json; not configured" >&2
    return 1
  fi

  local compiler_id
  compiler_id=$(python3 -c "
import json, sys
try:
    with open(sys.argv[1], 'r', encoding='utf-8') as f:
        data = json.load(f)
    print(data.get('host', {}).get('c', {}).get('id', ''))
except Exception:
    sys.exit(1)
" "${info_file}" 2>/dev/null || true)

  if [[ -z "${compiler_id}" ]]; then
    echo "error: failed to inspect compiler id from '${info_file}'" >&2
    return 1
  fi

  case "${compiler_id}" in
    gcc | clang)
      return 0
      ;;
    *)
      echo "error: build directory '${build_dir}' was configured with unsupported compiler '${compiler_id}'" >&2
      echo "       Netflix golden gate requires an explicitly supported compiler (gcc or clang) to prevent floating-point drift." >&2
      return 1
      ;;
  esac
}

if [[ "${1:-}" == "--check-compiler" ]]; then
  if [[ -z "${2:-}" ]]; then
    echo "Usage: $0 --check-compiler <build_dir>" >&2
    exit 2
  fi
  validate_compiler_id "$2"
  exit $?
fi

BUILD_DIR="${1:-core/build-golden}"
SOURCE_DIR="${2:-core}"
NINJA_BIN="${3:-ninja}"

if [[ -f "${BUILD_DIR}/build.ninja" ]]; then
  # Build directory exists; ensure it was configured with a supported compiler
  validate_compiler_id "${BUILD_DIR}"
else
  # Build directory does not exist or is incomplete; probe supported compiler
  if [[ -n "${GOLDEN_CC:-}" ]]; then
    if ! command -v "${GOLDEN_CC}" >/dev/null 2>&1; then
      echo "error: specified compiler GOLDEN_CC='${GOLDEN_CC}' not found" >&2
      exit 1
    fi
    CC_TO_USE="${GOLDEN_CC}"
    CXX_TO_USE="${GOLDEN_CXX:-${GOLDEN_CC}++}"
  elif command -v gcc >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
    CC_TO_USE="gcc"
    CXX_TO_USE="g++"
  elif command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
    CC_TO_USE="clang"
    CXX_TO_USE="clang++"
  else
    echo "error: No supported compiler (gcc or clang) found for deterministic golden build profile." >&2
    echo "       Netflix golden gate requires gcc or clang to prevent floating-point drift." >&2
    exit 1
  fi

  mkdir -p "${BUILD_DIR}"
  CC="${CC_TO_USE}" CXX="${CXX_TO_USE}" meson setup "${BUILD_DIR}" "${SOURCE_DIR}" \
    --buildtype release \
    -Denable_float=true \
    -Denable_cuda=false \
    -Denable_sycl=false \
    -Denable_hip=false \
    -Denable_dnn=disabled \
    -Denable_tests=false \
    -Denable_docs=false

  validate_compiler_id "${BUILD_DIR}"
fi

"${NINJA_BIN}" -vC "${BUILD_DIR}" tools/vmaf
