#!/usr/bin/env bash
# Test harness for scripts/ci/twin-drift-check.sh (ADR-1135).
#
# Builds throw-away git repos with fixture build files + sources, runs the
# gate inside each, and asserts on exit status plus output substrings.
# Hermetic: nothing outside `mktemp -d` is touched.
#
# Usage: bash scripts/ci/tests/test-twin-drift-check.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
GATE="$SCRIPT_DIR/../twin-drift-check.sh"

if [[ ! -f "$GATE" ]]; then
  printf 'ERROR: %s not found\n' "$GATE" >&2
  exit 1
fi

TMPDIR_TESTS="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_TESTS"' EXIT

pass=0
fail=0
repo=""

# new_repo — start a fresh fixture repo; subsequent add_file calls target it.
new_repo() {
  repo="$(mktemp -d -p "$TMPDIR_TESTS")"
  git -C "$repo" init -q
  git -C "$repo" config user.email "test@example.com"
  git -C "$repo" config user.name "Test"
}

# add_file <relpath> <content>
add_file() {
  mkdir -p "$repo/$(dirname "$1")"
  printf '%s\n' "$2" >"$repo/$1"
}

# run_case <desc> <expected-exit> <required-substring> [forbidden-substring]
run_case() {
  local desc="$1" want="$2" need="$3" forbid="${4:-}"
  git -C "$repo" add -A
  git -C "$repo" commit -q -m fixture
  local out rc=0
  out="$(cd "$repo" && bash "$GATE" 2>&1)" || rc=$?
  if [[ "$rc" -ne "$want" ]]; then
    printf 'FAIL: %s — exit %s, wanted %s:\n%s\n' "$desc" "$rc" "$want" "$out" >&2
    fail=$((fail + 1))
    return
  fi
  if ! grep -qF -- "$need" <<<"$out"; then
    printf 'FAIL: %s — output lacks %q:\n%s\n' "$desc" "$need" "$out" >&2
    fail=$((fail + 1))
    return
  fi
  if [[ -n "$forbid" ]] && grep -qF -- "$forbid" <<<"$out"; then
    printf 'FAIL: %s — output must not contain %q:\n%s\n' "$desc" "$forbid" "$out" >&2
    fail=$((fail + 1))
    return
  fi
  printf 'PASS: %s\n' "$desc"
  pass=$((pass + 1))
}

# Shared fixture: one production twin pair (src/log.c + src/log.cpp), the
# .cpp side compiled by src/meson.build via a directory variable, the .c
# side by test/meson.build via a relative path.
twin_fixture() {
  new_repo
  add_file src/log.c 'int log_c;'
  add_file src/log.cpp 'int log_cpp;'
  add_file src/meson.build "src_dir = './'
lib_sources = [src_dir + 'log.cpp']"
  add_file test/test.c 'int main(void) { return 0; }'
  add_file test/meson.build "t = executable('t', ['test.c', '../src/log.c'])"
}

# ---- (a) twin pairs -------------------------------------------------------

twin_fixture
run_case "T1 twin with both sides compiled passes" 0 "PASS:" "FAIL"

twin_fixture
add_file test/meson.build "t = executable('t', ['test.c'])"
run_case "T2 dead twin side fails" 1 "FAIL: dead twin side src/log.c is compiled by no build file"

twin_fixture
add_file test/meson.build "t = executable('t', ['test.c'])"
add_file scripts/ci/twin-drift-allowlist.txt "# comment line

src/log.c  legacy C twin kept until the C++23 port is wired in"
run_case "T3 allowlisted dead twin with a reason passes" 0 \
  "ALLOWLISTED: dead twin side src/log.c — legacy C twin kept" "FAIL"

twin_fixture
add_file test/meson.build "t = executable('t', ['test.c'])"
add_file scripts/ci/twin-drift-allowlist.txt "src/log.c"
run_case "T4 allowlist row without a reason fails" 1 "entry 'src/log.c' has no reason"

twin_fixture
add_file scripts/ci/twin-drift-allowlist.txt "src/log.c  stale row: the side is compiled again"
run_case "T5 stale allowlist row (side compiled again) fails" 1 \
  "'src/log.c' is now compiled by a build file — remove the row"

twin_fixture
add_file src/lone.c 'int lone;'
add_file src/meson.build "src_dir = './'
lib_sources = [src_dir + 'log.cpp', src_dir + 'lone.c']"
add_file scripts/ci/twin-drift-allowlist.txt "src/lone.c  not a twin at all"
run_case "T6 allowlist row for a non-twin file fails" 1 \
  "'src/lone.c' is not part of a .c/.cpp twin pair — remove the row"

twin_fixture
add_file scripts/ci/twin-drift-allowlist.txt "src/gone.c  file does not exist"
run_case "T7 allowlist row for an untracked file fails" 1 \
  "'src/gone.c' is not a tracked file — remove the row"

twin_fixture
add_file src/meson.build "src_dir = './'
lib_sources = [src_dir + 'log.cpp', src_dir + 'log.c']"
add_file test/test_x.c 'int tx;'
add_file test/test_x.cpp 'int txp;'
add_file test/meson.build "t = executable('t', ['test.c', 'test_x.c'])
u = executable('u', ['test.c', 'test_x.cpp'])"
run_case "T8 test-only INFO only for production sides" 0 \
  "PASS:" "INFO: test-only twin side test/"

twin_fixture
run_case "T9 production side compiled only by a test build file is reported INFO" 0 \
  "INFO: test-only twin side src/log.c — compiled only by test/meson.build"

# ---- (b) stale references -------------------------------------------------

twin_fixture
add_file test/meson.build "t = executable('t', ['test.c', '../src/log.c', '../src/gone.c'])"
run_case "T10 stale relative reference fails" 1 \
  "FAIL: stale source reference test/meson.build:1 '../src/gone.c' -> src/gone.c does not exist"

new_repo
add_file src/mem.cpp 'int m;'
add_file src/meson.build "src_dir = './'
lib_sources = [src_dir + 'mem.c']"
run_case "T11 rename fallout (mem.c -> mem.cpp) via directory variable fails" 1 \
  "src/meson.build:2 'mem.c' -> src/mem.c does not exist"

new_repo
add_file src/feature/adm.c 'int a;'
add_file src/meson.build "feature_dir = './feature/'
lib_sources = [feature_dir +
    'adm.c']"
run_case "T12 directory-variable prefix carried across a line break resolves" 0 \
  "1 exact + 0 suffix-searched" "FAIL"

new_repo
add_file core/src/mem.cpp 'int m;'
add_file pkg/core/ext.pyx 'cdef extern from "../../core/src/mem.cpp":
    pass'
add_file python/setup.py 'import os
PKG_REL = os.path.join("..", "pkg")
sources = [os.path.join("..", "core", "src", "mem.cpp"), os.path.join(PKG_REL, "core", "ext.pyx")]'
run_case "T13 setup.py os.path.join (literal + assigned variable) and .pyx extern resolve" 0 \
  "3 exact + 0 suffix-searched" "FAIL"

new_repo
add_file core/src/mem.cpp 'int m;'
add_file python/setup.py 'import os
sources = [os.path.join("..", "core", "src", "mem.c")]'
run_case "T14 stale setup.py os.path.join reference fails" 1 \
  "'../core/src/mem.c' -> core/src/mem.c does not exist"

new_repo
add_file core/src/mem.cpp 'int m;'
add_file python/setup.py 'import os
UNKNOWN = os.environ["X"]
sources = [os.path.join(UNKNOWN, "src", "mem.cpp")]'
run_case "T15 os.path.join with an unresolvable component falls back to suffix search" 0 \
  "resolved by suffix search -> 1 tracked file(s)" "FAIL"

new_repo
add_file pkg/core/ext.pyx 'cdef extern from "../../core/src/gone.c":
    pass'
run_case "T16 stale .pyx extern fails" 1 \
  "'../../core/src/gone.c' -> core/src/gone.c does not exist"

new_repo
add_file src/real.c 'int r;'
add_file src/meson.build "gen = custom_target('gen', input : 'tbl.json', output : 'gen_table.c', command : ['x'])
gen2 = custom_target('gen2', input : ['a.json'], output : ['@PLAINNAME@.c'], command : ['x'])
cfg = configure_file(output: 'config.c', configuration: cdata)
obj = custom_target('obj', output: name + '_hsaco.c', command : ['x'])
lib = library('l', ['real.c', gen])"
run_case "T17 generated outputs and @PLAINNAME@ substitutions are skipped" 0 \
  "1 exact + 0 suffix-searched" "FAIL"

new_repo
add_file src/real.c 'int r;'
add_file src/meson.build "# removed_long_ago.c was deleted in the C++23 port
lib = library('l', ['real.c']) # replaces old.c"
run_case "T18 comments are ignored" 0 "1 exact + 0 suffix-searched" "FAIL"

new_repo
add_file test/test.c 'int t;'
add_file test/test_psnr_parity.c 'int p;'
add_file test/test_ssim_parity.c 'int s;'
add_file test/meson.build "foreach _m : ['psnr', 'ssim']
  executable('t_' + _m, ['test.c', 'test_' + _m + '_parity.c'])
endforeach"
run_case "T19 loop-variable prefix resolves by suffix search" 0 \
  "NOTE: test/meson.build:2 _parity.c (prefix \`_m\` is not a literal directory) resolved by suffix search -> 2 tracked file(s)" "FAIL"

new_repo
add_file test/test.c 'int t;'
add_file test/meson.build "foreach _m : ['psnr']
  executable('t_' + _m, ['test.c', 'test_' + _m + '_parity.c'])
endforeach"
run_case "T20 loop-variable prefix with no matching file fails" 1 \
  "FAIL: unresolved source reference test/meson.build:2"

new_repo
add_file src/real.c 'int r;'
add_file src/meson.build "lib = library('l', ['real.c', 'later.c'])  # twin-drift-ignore: later.c is unpacked by a generator the parser cannot model"
run_case "T21 twin-drift-ignore with a reason skips the line" 0 \
  "skipped via twin-drift-ignore (later.c is unpacked by a generator" "FAIL"

new_repo
add_file src/real.c 'int r;'
add_file src/meson.build "lib = library('l', ['real.c', 'later.c'])  # twin-drift-ignore:"
run_case "T22 twin-drift-ignore without a reason does not suppress the finding" 1 \
  "FAIL: stale source reference src/meson.build:1 'later.c'"

new_repo
add_file src/real.c 'int r;'
add_file src/meson.build "sys_src = '/usr/share/foo/bar.c'
lib = library('l', ['real.c'])"
run_case "T23 absolute (toolchain) paths are skipped" 0 "1 exact + 0 suffix-searched" "FAIL"

new_repo
add_file README.md 'no build files here'
run_case "T24 repo without build files is skipped" 0 "no build files"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
