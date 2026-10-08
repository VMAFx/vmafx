#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# scripts/ci/werror-args.sh -- the one place a CI leg turns warnings into errors (ADR-2170).
#
#   meson setup core core/build ... $(scripts/ci/werror-args.sh "${{ matrix.werror }}")
#
# With `true` it prints the Meson arguments that make every compiler diagnostic and every
# linker diagnostic of the build fatal:
#   -Dwerror=true                         -Werror on every C and C++ compile (Meson's own switch)
#   -Dc_link_args / -Dcpp_link_args       the linker's fatal-warnings switch: GNU ld, lld and the
#                                         MinGW linker take --fatal-warnings, Apple's ld64
#                                         takes -fatal_warnings plus -no_warn_duplicate_libraries
#                                         (Meson's own -lc++ probe, see below)
# With `msvc` (a leg that builds with cl.exe and link.exe) it prints `-Dwerror=true` alone:
# Meson turns that into /WX on every cl.exe compile, -WX on every link.exe link (Meson 1.12
# adds the linker's fatal-warnings switch itself whenever werror is set) and, through
# core/src/meson.build, `--Werror all-warnings` on every nvcc fatbin. lib.exe, which archives
# the static libraries, has no switch in Meson; its warnings are counted in the leg's log.
# The MSVC legs run their steps under cmd and call this script from a `shell: bash` step.
# With anything else (empty, `false`, a matrix key that is not set) it prints nothing, so a leg
# that is not at zero warnings yet stays as it was. Any other value is a typo in a workflow and
# exits 2 rather than silently leaving the leg ungated.
#
# A leg gates itself only after it printed no warnings (docs/development/ci.md lists which).
# Release and container image builds do not use this script: a newer compiler than the one a
# leg pins must not stop a release over a new diagnostic.
set -euo pipefail

mode="${1:-}"
case "${mode}" in
  true) ;;
  msvc)
    printf '%s\n' "-Dwerror=true"
    exit 0
    ;;
  "" | false) exit 0 ;;
  *)
    echo "werror-args.sh: expected true, msvc, false or empty, got '${mode}'" >&2
    exit 2
    ;;
esac

# WERROR_ARGS_OS exists for the script's own test; a workflow never sets it.
os="${WERROR_ARGS_OS:-$(uname -s)}"
case "${os}" in
  # Meson probes the C++ runtime of a C-linked target with `clang++ ... -lc++`, which names
  # libc++ twice (the driver adds it too); ld64 warns "ignoring duplicate libraries: '-lc++'"
  # and -fatal_warnings turned that probe into "Could not detect either libc++ or libstdc++".
  # The switch below drops that one warning class, which no source can cause, and nothing else.
  Darwin) fatal="-Wl,-fatal_warnings,-no_warn_duplicate_libraries" ;;
  Linux | MINGW* | MSYS* | CYGWIN*) fatal="-Wl,--fatal-warnings" ;;
  *)
    echo "werror-args.sh: no fatal-warnings linker switch known for '${os}'" >&2
    exit 2
    ;;
esac

printf '%s\n' "-Dwerror=true" "-Dc_link_args=${fatal}" "-Dcpp_link_args=${fatal}"
