#!/usr/bin/env bash
# T7-13(a) — emit oneAPI environment variables for a chosen install version.
#
# Background: the local 2025.0.4 install at /opt/intel/oneapi/ ships device
# images that no longer match the Level Zero loader on rolling distros, so
# `vmaf_bench` has to resolve to the side-by-side 2025.3 install at
# /opt/intel/oneapi-2025.3/. Sourcing `setvars.sh` works for the current shell
# but does not survive `make` / `meson` invocations that fork a fresh shell.
#
# Usage:
#   eval "$(scripts/ci/sycl-bench-env.sh 2025.3)"
#   ./build-sycl/tools/vmaf_bench --device sycl ...
#
#   # Explicit prefix override:
#   ONEAPI_PREFIX=/opt/intel/oneapi-custom \
#     eval "$(scripts/ci/sycl-bench-env.sh 2025.3)"
#
# The script prints `export VAR=...` lines to stdout. Diagnostics go to
# stderr so `eval "$(...)"` does not break.
#
# Exit codes:
#   0 — environment lines emitted.
#   1 — version arg missing or install path not found.
#
# See docs/development/oneapi-install.md §"Multi-version coexistence".

set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <version>           e.g. 2025.3 | 2025.0 | latest" >&2
  echo "       $0 <version> --quiet   suppress diagnostic stderr" >&2
  exit 1
fi

VERSION="$1"
QUIET=0
if [ "${2:-}" = "--quiet" ]; then
  QUIET=1
fi

# Resolve the install root. Try the version-suffixed side-by-side install
# first, then the unversioned default install.
if [ -n "${ONEAPI_PREFIX:-}" ]; then
  ROOT="$ONEAPI_PREFIX"
elif [ "$VERSION" = "latest" ] && [ -d /opt/intel/oneapi ]; then
  ROOT="/opt/intel/oneapi"
elif [ -d "/opt/intel/oneapi-${VERSION}" ]; then
  ROOT="/opt/intel/oneapi-${VERSION}"
elif [ -d "/opt/intel/oneapi/${VERSION}" ]; then
  ROOT="/opt/intel/oneapi/${VERSION}"
elif [ -d /opt/intel/oneapi ]; then
  # Fallback: default install. Caller asked for a specific version but
  # only the unversioned tree exists — surface the mismatch.
  if [ "$QUIET" -eq 0 ]; then
    echo "warning: requested version '$VERSION' not found side-by-side;" >&2
    echo "         falling back to /opt/intel/oneapi (whatever it is)." >&2
  fi
  ROOT="/opt/intel/oneapi"
else
  echo "error: no oneAPI install found at /opt/intel/oneapi-$VERSION or /opt/intel/oneapi" >&2
  echo "       see docs/development/oneapi-install.md for the install recipe." >&2
  exit 1
fi

if [ ! -f "$ROOT/setvars.sh" ]; then
  echo "error: $ROOT/setvars.sh missing — install looks broken." >&2
  exit 1
fi

# Source setvars.sh in a subshell, then emit the resolved variables. This
# avoids polluting the caller with the ~40 oneAPI variables setvars.sh sets;
# we only forward the four bench/lint care about.
#
# The `--force` flag silences setvars.sh's "already initialized" notice when
# a prior install was sourced earlier in the parent shell.
ENV_DUMP=$(
  bash -c "
    set -e
    # shellcheck disable=SC1090
    source '$ROOT/setvars.sh' --force >/dev/null 2>&1 || true
    printf 'CMPLR_ROOT=%s\n'      \"\${CMPLR_ROOT:-}\"
    printf 'LD_LIBRARY_PATH=%s\n' \"\${LD_LIBRARY_PATH:-}\"
    printf 'LIBRARY_PATH=%s\n'    \"\${LIBRARY_PATH:-}\"
    printf 'PATH=%s\n'            \"\${PATH:-}\"
  "
)

while IFS= read -r line; do
  case "$line" in
    *=) ;; # empty value — skip
    *)
      key="${line%%=*}"
      val="${line#*=}"
      printf 'export %s=%q\n' "$key" "$val"
      ;;
  esac
done <<<"$ENV_DUMP"

if [ "$QUIET" -eq 0 ]; then
  echo "# oneAPI env activated from: $ROOT" >&2
fi
