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
# fails the stage. Run with `set -e`, a probe that finds nothing must
# not end the script, and a probe that finds something must still be reported.
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
# core/meson.build an unguarded use fails; with it the use passes; the define
# only in test_args, or commented out, is not project-wide and still fails.
printf '#include <math.h>\ndouble half_turn(void) { return M_PI; }\n' >uses_pi.c
mkdir -p core
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 1 ] || fail "M_PI without any define: expected rc=1, got $rc: $out"
case "$out" in *"M_* math macro without a _USE_MATH_DEFINES define"*"uses_pi.c:2:"*) ;;
*) fail "M_PI finding not printed: $out" ;; esac
echo "ok   an unguarded M_PI without the project-wide define fails the stage"

cat >core/meson.build <<'EOF_MESON'
if host_machine.system() == 'windows'
    test_args += '-D_USE_MATH_DEFINES'
    add_project_arguments('-D_USE_MATH_DEFINES', language: ['c', 'cpp'])
endif
EOF_MESON
rc=0
out="$(run_stage)" || rc=$?
[ "$rc" -eq 0 ] || fail "M_PI with the project-wide define: expected rc=0, got $rc: $out"
echo "ok   M_PI passes while core/meson.build defines _USE_MATH_DEFINES project-wide"

for planted in "    test_args += '-D_USE_MATH_DEFINES'" \
  "    # add_project_arguments('-D_USE_MATH_DEFINES', language: ['c', 'cpp'])" \
  "    add_project_arguments('-D_USE_MATH_DEFINES', language: ['cpp'])"; do
  printf "if host_machine.system() == 'windows'\n%s\nendif\n" "$planted" >core/meson.build
  rc=0
  out="$(run_stage)" || rc=$?
  [ "$rc" -eq 1 ] || fail "planted '$planted': expected rc=1, got $rc: $out"
done
rm -r core uses_pi.c
echo "ok   a define in test_args only, commented out or C++ only does not count"

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
rm -r fakebin
echo "ok   a POSIX-header scan that cannot run fails the stage"
