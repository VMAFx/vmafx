#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Tests for the msvcism stage of scripts/dev/preflight.sh in a throwaway repo:
# a tree with no hostile construct passes, and each planted construct (a
# `nullptr` in a C file, a parenthesised __attribute__, a POSIX-only header
# outside a platform conditional in a source the Windows build compiles, an
# M_PI without the project-wide _USE_MATH_DEFINES of core/meson.build) makes
# the stage fail with the finding printed; a guarded include and a source a
# meson.build keeps off Windows pass, and a POSIX-header scan that cannot run
# fails the stage. A `std::min()` call that a C++ translation unit reads behind
# `<windows.h>` (through the tree's own `<pthread.h>` shim, or directly under
# `#ifdef _WIN32`) fails the stage; the parenthesised spelling, a `NOMINMAX`
# before the header, an include Windows never reads, a call above the include
# and a translation unit a meson.build keeps off Windows pass. Run with
# `set -e`, a probe that finds nothing must not end the script, and a probe
# that finds something must still be reported.
set -euo pipefail

# Drop the git hook environment and the fixture identity (see the helper).
# shellcheck source=/dev/null
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/../../lib/clean-git-env.sh"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
preflight="$here/../../dev/preflight.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail() {
  echo "FAIL $1" >&2
  exit 1
}

cd "$tmp"
git init -q
printf 'int main(void) { return 0; }\n' >ok.c
git add ok.c
git commit -qm init

run_stage() {
  PREFLIGHT_BASE=HEAD bash "$preflight" --stage msvcism 2>&1
}

rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 0 ] || fail "clean tree failed (rc=$rc): $out"
case "$out" in *"PASS"*msvcism*) ;; *) fail "clean tree did not report PASS: $out" ;; esac
echo "ok   a tree without hostile constructs passes"

printf 'void *p = nullptr;\n' >bad_nullptr.c
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 1 ] || fail "nullptr in a C file: expected rc=1, got $rc: $out"
case "$out" in *"nullptr in a C translation unit"*) ;; *) fail "nullptr finding not printed: $out" ;; esac
rm bad_nullptr.c
echo "ok   a nullptr in a C file fails the stage and is reported"

printf '__attribute__(packed) int x;\n' >bad_attr.c
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 1 ] || fail "__attribute__(x): expected rc=1, got $rc: $out"
case "$out" in *"__attribute__ with single parens"*) ;; *) fail "attribute finding not printed: $out" ;; esac
echo "ok   a single-paren __attribute__ fails the stage and is reported"
rm bad_attr.c

printf '#include <unistd.h>\nint f(void) { return 0; }\n' >bad_posix.c
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 1 ] || fail "unguarded <unistd.h>: expected rc=1, got $rc: $out"
case "$out" in *"POSIX-only header outside a platform conditional"*"bad_posix.c:1: <unistd.h>"*) ;;
*) fail "POSIX header finding not printed: $out" ;; esac
echo "ok   an unguarded <unistd.h> in a source the Windows build compiles fails the stage"

printf '#ifndef _WIN32\n#include <unistd.h>\n#endif\nint f(void) { return 0; }\n' >bad_posix.c
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 0 ] || fail "guarded <unistd.h>: expected rc=0, got $rc: $out"
echo "ok   a <unistd.h> under #ifndef _WIN32 passes"

printf '#include <sys/socket.h>\nint g(void) { return 0; }\n' >posix_tool.c
printf "if host_machine.system() != 'windows'\n  executable('posix_tool', 'posix_tool.c')\nendif\n" >meson.build
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 0 ] || fail "source kept off Windows: expected rc=0, got $rc: $out"
printf "executable('posix_tool', 'posix_tool.c')\n" >meson.build
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 1 ] || fail "source built on Windows: expected rc=1, got $rc: $out"
case "$out" in *"posix_tool.c:1: <sys/socket.h>"*) ;; *) fail "meson-built finding not printed: $out" ;; esac
rm meson.build posix_tool.c
echo "ok   a meson.build gate off Windows exempts a source, an ungated target does not"

# M_PI needs _USE_MATH_DEFINES on Windows. Without the project-wide define in
# core/meson.build an unguarded use fails; with it, and with the same list on
# both SYCL argument lists of core/src/meson.build (icpx runs in custom
# targets, which project arguments never reach), the use passes. The define
# only in test_args, commented out, C++ only, as a literal outside the list, or
# missing from either SYCL list is not project-wide and still fails.
printf '#include <math.h>\ndouble half_turn(void) { return M_PI; }\n' >uses_pi.c
mkdir -p core/src
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 1 ] || fail "M_PI without any define: expected rc=1, got $rc: $out"
case "$out" in *"M_* math macro without a _USE_MATH_DEFINES define"*"uses_pi.c:2:"*) ;;
*) fail "M_PI finding not printed: $out" ;; esac
echo "ok   an unguarded M_PI without the project-wide define fails the stage"

write_math_meson() { # $1: the windows branch of core/meson.build
  printf "vmaf_math_constant_args = []\nif host_machine.system() == 'windows'\n%s\nendif\n" "$1" \
    >core/meson.build
}
write_sycl_meson() { # $1, $2: the tails of sycl_common_args and sycl_feature_tail_args
  printf "    sycl_common_args = vmaf_cflags_common + ['-c'] \\\\\n        %s\n" "$1" >core/src/meson.build
  printf "    sycl_feature_tail_args = ['-std=c++20'] \\\\\n        %s\n" "$2" >>core/src/meson.build
}
good_branch="    vmaf_math_constant_args = ['-D_USE_MATH_DEFINES']
    test_args += vmaf_math_constant_args
    add_project_arguments(vmaf_math_constant_args, language: ['c', 'cpp'])"
write_math_meson "$good_branch"
write_sycl_meson "+ vmaf_math_constant_args" "+ vmaf_math_constant_args"
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 0 ] || fail "M_PI with the project-wide define: expected rc=0, got $rc: $out"
echo "ok   M_PI passes while the define is project-wide and on both SYCL lists"

for planted in "    vmaf_math_constant_args = ['-D_USE_MATH_DEFINES']
    test_args += vmaf_math_constant_args" \
  "    vmaf_math_constant_args = ['-D_USE_MATH_DEFINES']
    # add_project_arguments(vmaf_math_constant_args, language: ['c', 'cpp'])" \
  "    vmaf_math_constant_args = ['-D_USE_MATH_DEFINES']
    add_project_arguments(vmaf_math_constant_args, language: ['cpp'])" \
  "    add_project_arguments('-D_USE_MATH_DEFINES', language: ['c', 'cpp'])"; do
  write_math_meson "$planted"
  rc=0
  out="$(run_stage)" || rc=$?
  [ "$rc" -eq 1 ] || fail "planted '$planted': expected rc=1, got $rc: $out"
done
echo "ok   a define in test_args only, commented out, C++ only or outside the list does not count"

write_math_meson "$good_branch"
for lists in "+ []|+ vmaf_math_constant_args" "+ vmaf_math_constant_args|+ []"; do
  write_sycl_meson "${lists%%|*}" "${lists#*|}"
  rc=0
  out="$(run_stage)" || rc=$?
  [ "$rc" -eq 1 ] || fail "SYCL lists '$lists': expected rc=1, got $rc: $out"
done
rm -r core uses_pi.c
echo "ok   a SYCL argument list without the define does not count"

# <windows.h> defines min and max as function-like macros unless NOMINMAX comes
# first, and <pthread.h> is core/src/compat/win32/pthread.h on Windows, which
# includes it. A std::min() call a C++ translation unit reads after that fails
# under MSVC (C2589) and nowhere else.
mkdir -p core/src/compat/win32
printf '#include <windows.h>\n' >core/src/compat/win32/pthread.h
write_minmax_unit() { # $1: the lines of unit.cpp above the header that calls std::min
  printf '%b#include "cap.h"\nint main() { return cap(1, 2); }\n' "$1" >unit.cpp
}
write_cap_header() { # $1: the call
  printf '#include <algorithm>\ninline int cap(int a, int b) { return %s; }\n' "$1" >cap.h
}
expect_minmax() { # $1: expected rc, $2: what the case plants
  rc=0
  out="$(run_stage)" || rc=$?
  [ "$rc" -eq "$1" ] || fail "$2: expected rc=$1, got $rc: $out"
  if [ "$1" -eq 1 ]; then
    case "$out" in *"call behind <windows.h> without NOMINMAX"*"cap.h:2: ::min( behind <windows.h> (reached from unit.cpp)"*) ;;
    *) fail "$2: min/max finding not printed: $out" ;; esac
  else
    case "$out" in *"behind <windows.h>"*) fail "$2: reported a finding: $out" ;; *) ;; esac
  fi
}

write_cap_header 'std::min(a, b)'
write_minmax_unit '#include <pthread.h>\n'
expect_minmax 1 "std::min() behind the <pthread.h> shim"
echo "ok   a std::min() call behind the <pthread.h> shim fails the stage and is reported"

write_minmax_unit '#ifdef _WIN32\n#include <windows.h>\n#endif\n'
expect_minmax 1 "std::min() behind <windows.h> under #ifdef _WIN32"
echo "ok   a std::min() call behind <windows.h> under #ifdef _WIN32 fails the stage"

write_cap_header '(std::min)(a, b)'
write_minmax_unit '#include <pthread.h>\n'
expect_minmax 0 "(std::min)(a, b) behind the shim"
write_cap_header 'std::min<int>(a, b)'
expect_minmax 0 "std::min<int>(a, b) behind the shim"
echo "ok   (std::min)(a, b) and std::min<int>(a, b) behind the header pass"

write_cap_header 'std::min(a, b)'
write_minmax_unit '#define NOMINMAX\n#include <pthread.h>\n'
expect_minmax 0 "NOMINMAX before the header"
echo "ok   a NOMINMAX defined before the header passes"

write_minmax_unit '#ifndef _WIN32\n#include <pthread.h>\n#endif\n'
expect_minmax 0 "<pthread.h> under #ifndef _WIN32"
write_minmax_unit '#include <vector>\n'
expect_minmax 0 "no Windows header"
echo "ok   an include Windows never reads and a unit without the header pass"

printf '#include "cap.h"\n#include <pthread.h>\nint main() { return cap(1, 2); }\n' >unit.cpp
expect_minmax 0 "the call above the include"
echo "ok   a call read before the header passes"

write_minmax_unit '#include <pthread.h>\n'
printf "if host_machine.system() != 'windows'\n  executable('unit', 'unit.cpp')\nendif\n" >meson.build
expect_minmax 0 "translation unit kept off Windows"
printf "executable('unit', 'unit.cpp')\n" >meson.build
expect_minmax 1 "translation unit built on Windows"
rm -r core meson.build unit.cpp cap.h
echo "ok   a meson.build gate off Windows exempts a translation unit, an ungated target does not"

# Many findings: a probe trimmed with `head` closed the pipe while the scan
# still wrote, the scan died of SIGPIPE, and under pipefail the `|| x=""`
# fallback threw the findings away. They must fail the stage.
for i in $(seq 1 60); do
  printf '#include <math.h>\ndouble f%s(void) { return M_PI; }\ndouble g%s(void) { return M_PI; }\n' \
    "$i" "$i" >"many_pi_$i.c"
done
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 1 ] || fail "60 unguarded M_PI files: expected rc=1, got $rc: $out"
case "$out" in *"M_* math macro without a _USE_MATH_DEFINES define"*) ;; *) fail "many-findings case not printed: $out" ;; esac
rm many_pi_*.c
echo "ok   sixty files of findings still fail the stage"

# A scanner that cannot run must fail the stage, not pass it for want of output.
mkdir -p fakebin
printf '#!/bin/sh\nexit 3\n' >fakebin/python3
chmod +x fakebin/python3
rc=0
out="$(PATH="$tmp/fakebin:$PATH" run_stage)" || rc=$?
[ "$rc" -eq 1 ] || fail "failing scanner: expected rc=1, got $rc: $out"
case "$out" in *"POSIX-only header scan did not run"*) ;; *) fail "scanner failure not reported: $out" ;; esac
case "$out" in *"Windows min/max macro scan did not run"*) ;; *) fail "min/max scanner failure not reported: $out" ;; esac
rm -r fakebin
echo "ok   a POSIX-header scan and a min/max scan that cannot run fail the stage"
