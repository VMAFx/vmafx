#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# ADR-0105 copyright-header enforcement + ADR-1250 SPDX-identifier enforcement.
#
# Policy:
#   - ADR-0105: every fork-added C/C++/CUDA source or header ships one of
#     three copyright templates (Netflix-only, Lusoris+Claude-only, Dual notice).
#   - ADR-1250: source files of the languages named in ADR-1250 (C, C++, CUDA,
#     Go, Python) must carry a valid SPDX-License-Identifier line.
#
# This pre-commit hook enforces:
#   1. The presence of a Copyright line on each staged C/C++/CUDA source file.
#   2. The presence of an SPDX-License-Identifier line on each staged file of
#      the languages named in ADR-1250.
#
# Scope: C, C++, CUDA, Go, Python source and header files passed as CLI args.
#
# Exit 0 on pass, 1 on any missing Copyright header or SPDX identifier.

set -euo pipefail

fail=0

for f in "$@"; do
  [ -f "$f" ] || continue

  # Skip auto-generated files (e.g. meson's config.h.in, bison/flex
  # output), upstream Netflix training-harness MATLAB MEX sources
  # that never carry a copyright/SPDX header by convention, and
  # vendored Pelorus mirror paths (ADR-1113 / ADR-1276).
  case "$f" in
    *config.h.in | *generated* | *compat/python-vmaf/matlab/* | *core/include/libvmaf/pelorus/* | *core/src/interop/pelorus* | *core/test/test_pelorus_interop.c) continue ;;
  esac

  # ADR-0105: Check Copyright header on C/C++/CUDA files.
  case "$f" in
    *.c | *.h | *.cpp | *.cxx | *.cc | *.hpp | *.hxx | *.cu | *.cuh | *.hip)
      if ! head -n 40 "$f" 2>/dev/null | grep -qi 'copyright'; then
        echo "ADR-0105: $f missing Copyright header" >&2
        fail=1
      fi
      ;;
  esac

  # ADR-1250: Check SPDX-License-Identifier line on languages ADR-1250 names.
  case "$f" in
    *.c | *.h | *.cpp | *.cxx | *.cc | *.hpp | *.hxx | *.cu | *.cuh | *.hip | *.go | *.py)
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
