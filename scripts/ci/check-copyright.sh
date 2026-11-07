#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# ADR-0105 copyright-header enforcement + ADR-1250 SPDX-identifier enforcement.
#
# Policy:
#   - ADR-0105: every fork-added C/C++/CUDA/HIP/Metal source or header ships one of
#     three copyright templates (Netflix-only, Lusoris-only (ADR-0861), Dual notice).
#   - ADR-1250: source files of the languages named in ADR-1250 (C, C++, CUDA, HIP,
#     Metal, Objective-C++, Go, Python, Cython, Rust, shell) must carry a valid
#     SPDX-License-Identifier line.
#
# This pre-commit hook enforces:
#   1. The presence of a Copyright line on each staged C-family source file.
#   2. The presence of an SPDX-License-Identifier line on each staged file of
#      the languages named in ADR-1250.
#
# A file that cannot meet a rule is named in the declared exception list
# (.config/lint-exceptions.d/{copyright,spdx}.toml, scripts/ci/lint_exceptions.py): one
# file, one rule, a reason and an expiry. An expired entry no longer holds. The script has
# no other skip.
#
# Exit 0 on pass, 1 on any missing Copyright header or SPDX identifier.

set -euo pipefail

fail=0
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Files each rule still reads: the arguments minus the live exceptions of that rule.
declared_filter() {
  local rule="$1"
  shift
  python3 "$here/lint_exceptions.py" filter "$rule" -- "$@"
}

copyright_files=()
spdx_files=()
if [ "$#" -gt 0 ]; then
  mapfile -t copyright_files < <(declared_filter copyright "$@")
  mapfile -t spdx_files < <(declared_filter spdx "$@")
fi

for f in "${copyright_files[@]+"${copyright_files[@]}"}"; do
  [ -f "$f" ] || continue
  # ADR-0105: Check Copyright header on C-family files.
  case "$f" in
    *.c | *.h | *.cpp | *.cxx | *.cc | *.hpp | *.hxx | *.cu | *.cuh | *.hip | *.mm | *.metal)
      if ! head -n 40 "$f" 2>/dev/null | grep -qi 'copyright'; then
        echo "ADR-0105: $f missing Copyright header" >&2
        fail=1
      fi
      ;;
  esac
done

for f in "${spdx_files[@]+"${spdx_files[@]}"}"; do
  [ -f "$f" ] || continue
  # ADR-1250: Check SPDX-License-Identifier line on languages ADR-1250 names.
  case "$f" in
    *.c | *.h | *.cpp | *.cxx | *.cc | *.hpp | *.hxx | *.cu | *.cuh | *.hip | *.mm | *.metal | *.go | *.py | *.pyx | *.rs | *.sh)
      # REUSE-IgnoreStart
      if ! head -n 40 "$f" 2>/dev/null | grep -E -q 'SPDX-License-Identifier:[[:space:]]+[A-Za-z0-9]'; then
        echo "ADR-1250: $f missing SPDX-License-Identifier" >&2
        fail=1
      fi
      # REUSE-IgnoreEnd
      ;;
  esac
done

exit "$fail"
