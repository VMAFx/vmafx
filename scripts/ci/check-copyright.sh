#!/usr/bin/env bash
# ADR-0105 copyright-header enforcement.
#
# Policy: every fork-added C/C++/CUDA source or header ships one of
# three copyright templates:
#   (a) Netflix-only — pre-existing upstream paths, year range 2016-2026
#   (b) Lusoris+Claude-only — wholly fork-authored subtrees (e.g. SYCL)
#   (c) Dual notice — fork-modified Netflix sources
#
# This pre-commit hook enforces the *presence* of any Copyright line on
# each staged source file. Template correctness (which of the three
# applies) remains a reviewer judgement — the year-range window moves
# and the "fork-authored vs upstream-modified" split is not derivable
# from the diff alone.
#
# Scope: *.c / *.h / *.cpp / *.cxx / *.cc / *.hpp / *.hxx / *.cu / *.cuh
# passed as CLI args by pre-commit.
#
# Exit 0 on pass, 1 on any missing-Copyright file.

set -euo pipefail

fail=0

for f in "$@"; do
  [ -f "$f" ] || continue

  # Skip auto-generated files (e.g. meson's config.h.in, bison/flex
  # output) that never carry a copyright header by convention.
  case "$f" in
    *config.h.in | *generated*) continue ;;
  esac

  # Check the first 40 lines — covers every existing header in the
  # tree (longest is upstream Netflix's multi-line banner at ~18 lines).
  if ! head -n 40 "$f" 2>/dev/null | grep -qi 'copyright'; then
    echo "ADR-0105: $f missing Copyright header" >&2
    fail=1
  fi
done

exit "$fail"
