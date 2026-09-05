#!/usr/bin/env bash
# T7-13(b) — icpx-aware clang-tidy wrapper.
#
# Stock LLVM clang-tidy does not pick up Intel oneAPI's SYCL include path
# the way `icpx` does, so direct invocations against SYCL TUs report
# `'sycl/sycl.hpp' file not found` clang-diagnostic-errors. This wrapper
# injects the SYCL include path + warning suppressions so the changed-file
# CI lint gate can cover files under core/src/sycl/ and core/src/feature/sycl/.
#
# Usage (mirrors clang-tidy):
#   scripts/ci/clang-tidy-sycl.sh -p <build-sycl-dir> [other args] <file>
#
# Environment:
#   ICPX_ROOT       — override the icpx install root.
#                     Defaults to /opt/intel/oneapi/compiler/latest/linux.
#   CLANG_TIDY_BIN  — override the underlying clang-tidy binary.
#                     Defaults to `clang-tidy` from PATH.
#
# Exit code = clang-tidy's exit code.
#
# See ADR-0217 for the design and docs/development/oneapi-install.md
# §"Verify SYCL clang-tidy still works" for the recipe context.

set -euo pipefail

# ---------------------------------------------------------------------
# Resolve clang-tidy binary.
# ---------------------------------------------------------------------
CLANG_TIDY_BIN="${CLANG_TIDY_BIN:-clang-tidy}"
if ! command -v "$CLANG_TIDY_BIN" >/dev/null 2>&1; then
  echo "error: clang-tidy binary '$CLANG_TIDY_BIN' not found on PATH" >&2
  echo "       set CLANG_TIDY_BIN to the desired clang-tidy executable." >&2
  exit 127
fi

# ---------------------------------------------------------------------
# Resolve icpx SYCL include path.
#
# Default: /opt/intel/oneapi/compiler/latest/include (Intel's 2025.x
# layout — `compiler/latest` is a symlink to the active version dir).
# Older / non-Linux installs use a `linux/` subcomponent; both shapes
# are tried. Set ICPX_ROOT to the directory containing `include/` if
# the wrapper can't auto-resolve.
# ---------------------------------------------------------------------
ICPX_ROOT="${ICPX_ROOT:-/opt/intel/oneapi/compiler/latest}"
SYCL_INCLUDE=""
for cand in \
  "$ICPX_ROOT/include/sycl" \
  "$ICPX_ROOT/include" \
  "$ICPX_ROOT/linux/include/sycl" \
  "$ICPX_ROOT/linux/include" \
  "/opt/intel/oneapi/compiler/latest/include/sycl" \
  "/opt/intel/oneapi/compiler/latest/include" \
  "/opt/intel/oneapi/compiler/latest/linux/include/sycl" \
  "/opt/intel/oneapi/compiler/latest/linux/include" \
  "/opt/intel/oneapi-2025.3/compiler/latest/include/sycl" \
  "/opt/intel/oneapi-2025.3/compiler/latest/include"; do
  if [ -f "$cand/sycl/sycl.hpp" ] || [ -f "$cand/sycl.hpp" ]; then
    SYCL_INCLUDE="$cand"
    break
  fi
done

if [ -z "$SYCL_INCLUDE" ]; then
  echo "error: cannot locate <sycl/sycl.hpp> under any oneAPI install." >&2
  echo "       set ICPX_ROOT to <oneapi>/compiler/latest/linux." >&2
  echo "       see docs/development/oneapi-install.md for install instructions." >&2
  exit 1
fi

# Strip a trailing "sycl" component so the include lands at the parent dir
# (sycl/sycl.hpp is the canonical header form).
case "$SYCL_INCLUDE" in
  */sycl) SYCL_INCLUDE_BASE="${SYCL_INCLUDE%/sycl}" ;;
  *) SYCL_INCLUDE_BASE="$SYCL_INCLUDE" ;;
esac

# ---------------------------------------------------------------------
# Build the clang-tidy invocation.
#
# -extra-arg-before=-std=c++20        — pin the language standard. icpx
#                                       uses C++20 for SYCL TUs in this repo.
#                                       Stock clang-tidy without a -std flag
#                                       falls back to C++11 defaults if not
#                                       present in compile_commands.json.
# -extra-arg-before=-isystem<dir>     — make <sycl/sycl.hpp> resolvable.
# -extra-arg-before=-Wno-unknown-warning-option
#                                     — suppress warnings for flags not
#                                       supported by clang.
# -extra-arg-before=-Wno-unknown-pragmas
#                                     — same rationale for icpx pragmas
#                                       (`#pragma clang fp ...` etc).
# ---------------------------------------------------------------------
exec "$CLANG_TIDY_BIN" \
  "-extra-arg-before=-std=c++20" \
  "-extra-arg-before=-isystem$SYCL_INCLUDE_BASE" \
  "-extra-arg-before=-isystem$SYCL_INCLUDE_BASE/sycl" \
  "-extra-arg-before=-Wno-unknown-warning-option" \
  "-extra-arg-before=-Wno-unknown-pragmas" \
  "$@"
