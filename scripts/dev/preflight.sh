#!/usr/bin/env bash
# preflight.sh — run the CI checks that a gcc-only local build cannot catch.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# The repository's documented local gate (`make lint`, `meson test`) builds with
# ONE compiler. CI builds with several, and the difference is not academic: on
# 2026-09-07 a single branch shipped three separate portability breaks that were
# green locally and red in CI, each costing a full round-trip on a queue where
# one PR at a time can be in flight:
#
#   * a `static_assert` on `UINT_MAX <= SIZE_MAX/2/sizeof(ptr)` — true on LP64,
#     FALSE on 32-bit, so `Ubuntu i686 gcc` would not compile the file;
#   * `#define ALIGNED(x) __declspec(align((x)))` — MSVC needs a literal there,
#     so `Windows MSVC+CUDA` failed C2059 on every use;
#   * `__attribute__(noinline)` (a paren accidentally stripped) — gcc accepted
#     it, clang did not, taking out `Ubuntu clang`, `Ubuntu clang+DNN` and all
#     four Sanitizer lanes at once.
#
# Every one of those is catchable in seconds on a developer machine. This script
# does that, cheapest check first so it fails fast.
#
# Usage:
#   scripts/dev/preflight.sh              # changed-files mode (vs origin/master)
#   scripts/dev/preflight.sh --full       # whole tree
#   scripts/dev/preflight.sh --stage NAME # one stage only
#   scripts/dev/preflight.sh --list       # show stages and what each mirrors
#
# Exit: 0 all stages passed (or were skipped for missing tooling), 1 a stage
# failed, 2 usage error. A missing toolchain SKIPS its stage with a notice
# rather than failing, so the script is useful on a partially-provisioned box.
set -uo pipefail
export LC_ALL=C

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT" || exit 2

BASE="${PREFLIGHT_BASE:-origin/master}"
MODE=changed
ONLY=""

while [ $# -gt 0 ]; do
  case "$1" in
    --full)
      MODE=full
      shift
      ;;
    --stage)
      ONLY="${2:-}"
      shift 2
      ;;
    --list)
      MODE=list
      shift
      ;;
    -h | --help)
      sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      printf 'preflight: unknown argument: %s\n' "$1" >&2
      exit 2
      ;;
  esac
done

pass=0
fail=0
skip=0
FAILED_STAGES=""

say() { printf '\n\033[1m== %s\033[0m  (%s)\n' "$1" "$2"; }
ok() {
  printf '   \033[32mPASS\033[0m %s\n' "$1"
  pass=$((pass + 1))
}
bad() {
  printf '   \033[31mFAIL\033[0m %s\n' "$1"
  fail=$((fail + 1))
  FAILED_STAGES="$FAILED_STAGES $1"
}
skipped() {
  printf '   \033[33mSKIP\033[0m %s — %s\n' "$1" "$2"
  skip=$((skip + 1))
}

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

# Committed work AND the working tree. A preflight that only looked at
# committed changes would miss the edit you are about to commit, which is the
# entire point of running it.
changed_sources() {
  if [ "$MODE" = full ]; then
    git ls-files '*.c' '*.cpp' '*.h' '*.hpp'
    return
  fi
  {
    git diff --name-only --diff-filter=d "$BASE"...HEAD -- '*.c' '*.cpp' '*.h' '*.hpp'
    git diff --name-only --diff-filter=d -- '*.c' '*.cpp' '*.h' '*.hpp'
    git diff --name-only --diff-filter=d --cached -- '*.c' '*.cpp' '*.h' '*.hpp'
    git ls-files --others --exclude-standard -- '*.c' '*.cpp' '*.h' '*.hpp'
  } | sort -u
}

if [ "$MODE" = list ]; then
  cat <<'EOF'
stage          mirrors CI context            catches
-----          ------------------            -------
gcc            Ubuntu gcc(+DNN)              the baseline build
clang          Ubuntu clang(+DNN)            clang-only syntax, e.g. __attribute__(x)
m32            Ubuntu i686 gcc               32-bit-only static_assert / size assumptions
msvcism        Windows MSVC+CUDA / +SYCL     constructs MSVC rejects (no MSVC needed)
sanitizers     Sanitizers (a/t/ub)           UB and races the plain build hides
tidy           Tidy Changed                  clang-tidy on the files this branch touches
cppcheck       Cppcheck                      cppcheck's own findings
EOF
  exit 0
fi

printf 'preflight: mode=%s base=%s\n' "$MODE" "$BASE"

# ---------------------------------------------------------------- gcc build --
if want gcc; then
  say "gcc build + fast tests" "mirrors: Ubuntu gcc"
  if [ -d build ] || CC=gcc CXX=g++ meson setup build core \
    -Denable_cuda=false -Denable_sycl=false -Db_lto=false >/dev/null 2>&1; then
    if ninja -C build >/tmp/preflight-gcc.log 2>&1 &&
      meson test -C build --suite=fast >>/tmp/preflight-gcc.log 2>&1; then
      ok gcc
    else
      bad gcc
      grep -m3 -A4 'FAILED:\|^Fail:' /tmp/preflight-gcc.log | head -12
    fi
  else
    skipped gcc "meson setup failed"
  fi
fi

# -------------------------------------------------------------- clang build --
# The lane that catches attribute and extension differences. gcc is famously
# permissive about both.
if want clang; then
  say "clang build + fast tests" "mirrors: Ubuntu clang, and every Sanitizer lane's compile step"
  if ! command -v clang >/dev/null; then
    skipped clang "clang not installed"
  elif [ -d build-clang ] || CC=clang CXX=clang++ meson setup build-clang core \
    -Denable_cuda=false -Denable_sycl=false -Db_lto=false >/dev/null 2>&1; then
    if ninja -C build-clang >/tmp/preflight-clang.log 2>&1 &&
      meson test -C build-clang --suite=fast >>/tmp/preflight-clang.log 2>&1; then
      ok clang
    else
      bad clang
      grep -m3 -A4 'error:\|^Fail:' /tmp/preflight-clang.log | head -12
    fi
  else
    skipped clang "meson setup failed"
  fi
fi

# ------------------------------------------------------------ 32-bit syntax --
# A full 32-bit build needs multilib libraries; -fsyntax-only needs only the
# compiler, and that is enough for the failure mode this exists for: an
# assumption about the width of size_t / ptrdiff_t / pointers.
if want m32; then
  say "32-bit syntax sweep" "mirrors: Ubuntu i686 gcc"
  if ! echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /tmp/preflight-m32probe 2>/dev/null; then
    skipped m32 "no 32-bit gcc support (install gcc-multilib)"
  elif [ ! -f build/src/config.h ]; then
    skipped m32 "build/src/config.h missing — run the gcc stage first"
  else
    m32_fail=0
    while IFS= read -r f; do
      [ -f "$f" ] || continue
      case "$f" in *.h | *.hpp) continue ;; esac
      if [ "${f%.cpp}" != "$f" ]; then
        std=(-std=c++23 -x c++)
      else
        std=(-std=c2x -x c)
      fi
      # -I build/src supplies the generated config.h. Without it the compile
      # aborts at the first #include and never reaches the code under test,
      # which silently turned this stage into a no-op.
      gcc -m32 -fsyntax-only "${std[@]}" -D_GNU_SOURCE \
        -I core/include -I core/src -I core/src/feature -I core/tools -I core/test \
        -I build/src -I build \
        "$f" 2>/tmp/preflight-m32.err
      # Missing generated headers (config.h) and absent intrinsics are expected
      # outside a configured build; width assumptions are not.
      if grep -qE 'error:' /tmp/preflight-m32.err &&
        ! grep -qE "config\.h|file not found|Datei oder Verzeichnis" /tmp/preflight-m32.err; then
        printf '     %s\n' "$f"
        grep -m2 -E 'error:' /tmp/preflight-m32.err | sed 's/^/       /'
        m32_fail=1
      fi
    done < <(changed_sources)
    if [ "$m32_fail" -eq 0 ]; then
      ok m32
    else
      bad m32
    fi
  fi
fi

# --------------------------------------------------------------- MSVC-isms --
# No MSVC on Linux, but its rejections are a small, well-known set and they are
# greppable. Each pattern below cost a real CI round-trip at least once.
if want msvcism; then
  say "MSVC-hostile constructs" "mirrors: Windows MSVC+CUDA / MSVC+SYCL (static approximation)"
  msvc_fail=0
  check_pattern() {
    local pat="$1" why="$2" hits
    hits=$(changed_sources | xargs -r grep -nE "$pat" 2>/dev/null | head -5)
    if [ -n "$hits" ]; then
      printf '     %s\n' "$why"
      printf '%s\n' "$hits" | sed 's/^/       /'
      msvc_fail=1
    fi
  }
  # __declspec(align(N)) and __attribute__((aligned(N))) take a literal; an
  # extra paren around the argument is a syntax error under MSVC.
  check_pattern 'align\(\([^)]' 'align() argument wrapped in parentheses — MSVC C2059'
  check_pattern 'declspec\(\([a-z]' '__declspec argument wrapped in parentheses'
  # __attribute__ needs its double parens; a single pair is a clang error.
  check_pattern '__attribute__\([a-z_]' '__attribute__ with single parens — clang rejects'

  # ADR-1138: C translation units keep `NULL`. MSVC's documented /std:clatest
  # C23 feature set does not implement the `nullptr` keyword, and the Windows
  # lane compiles core/ with cl.exe. gcc accepts it and clang accepts it under
  # -std=c23, so nothing local objects. Comment lines are skipped because the
  # ADR-1138 NOLINT bracket names the keyword in prose.
  c_nullptr=$(changed_sources | grep -E '\.c$' | while read -r f; do
    grep -nE '\bnullptr\b' "$f" 2>/dev/null |
      grep -vE '^[0-9]+:[[:space:]]*(\*|/\*|//)' | sed "s|^|$f:|"
  done | head -5)
  if [ -n "$c_nullptr" ]; then
    printf '     %s\n' 'nullptr in a C translation unit — MSVC C2065; ADR-1138 keeps C on NULL'
    printf '%s\n' "$c_nullptr" | sed 's/^/       /'
    msvc_fail=1
  fi

  # `M_PI` and friends are POSIX/X-Open, not ISO C. glibc exposes them only
  # under __USE_MISC/__USE_XOPEN, which `-std=c23` disables by defining
  # __STRICT_ANSI__; the Linux lanes see them anyway because meson passes
  # -D_GNU_SOURCE. MinGW64 does not honour _GNU_SOURCE, so a file that just
  # includes <math.h> and uses M_PI compiles everywhere except the required
  # `Windows MinGW64` lane. The tree's convention is _USE_MATH_DEFINES before
  # <math.h> plus an `#ifndef M_PI` fallback (adm_csf_tools.h, adm_tools.h).
  m_macros='\bM_(PI|E|SQRT2|LN2|LN10|PI_2|PI_4|1_PI|2_PI|SQRT1_2|LOG2E|LOG10E)\b'
  # A file counts as guarded when it carries the two-step itself OR includes an
  # in-tree header that does -- integer_adm.c, adm_avx2.c and friends get M_PI
  # from integer_adm.h / barten_csf_tools.h and are correct as written.
  c_math=$(changed_sources | while read -r f; do
    grep -qE "$m_macros" "$f" 2>/dev/null || continue
    grep -qE '_USE_MATH_DEFINES|#ifndef M_PI' "$f" 2>/dev/null && continue
    guarded_by_include=0
    while read -r h; do
      [ -n "$h" ] || continue
      if find core -name "$(basename "$h")" -exec \
        grep -lE '_USE_MATH_DEFINES|#ifndef M_PI' {} + 2>/dev/null | grep -q .; then
        guarded_by_include=1
        break
      fi
    done <<EOF_INC
$(grep -oE '#include "[^"]+"' "$f" 2>/dev/null | sed 's|#include "||; s|"||')
EOF_INC
    [ "$guarded_by_include" -eq 1 ] && continue
    grep -nE "$m_macros" "$f" | head -2 | sed "s|^|$f:|"
  done | head -5)
  if [ -n "$c_math" ]; then
    printf '     %s\n' 'M_* math macro without the _USE_MATH_DEFINES / #ifndef guard — MinGW64 C2065'
    printf '%s\n' "$c_math" | sed 's/^/       /'
    msvc_fail=1
  fi

  # A `static const double x = 1.0;` is a const-qualified OBJECT in C, not a
  # constant expression, so it may not initialise a `static` aggregate (MSVC
  # C2099, then cascading C2440s as the remaining initialisers shift into the
  # wrong members). gcc and clang accept it as an extension and emit no
  # diagnostic at all, not even under -std=c23 -pedantic-errors -Weverything,
  # so this is grepped rather than compiled.
  c_static_init=$(changed_sources | grep -E '\.c$' |
    xargs -r python3 "$REPO_ROOT/scripts/dev/find-nonconst-static-init.py" 2>/dev/null | head -5)
  if [ -n "$c_static_init" ]; then
    printf '     %s\n' 'non-constant initialiser in a static aggregate — MSVC C2099; use #define'
    printf '%s\n' "$c_static_init" | sed 's/^/       /'
    msvc_fail=1
  fi
  if [ "$msvc_fail" -eq 0 ]; then
    ok msvcism
  else
    bad msvcism
  fi
fi

# ------------------------------------------------------------- sanitizers --
if want sanitizers; then
  # -Db_lundef=false drops -Wl,--no-undefined. Clang links the sanitizer
  # runtime into executables, not shared libraries, so libvmaf.so is left with
  # undefined __asan_report_* / __ubsan_handle_* and --no-undefined rejects the
  # link. The repo's own fuzz workflow pairs the same two options.
  say "ASan + UBSan build" "mirrors: Sanitizers (address) / (undefined)"
  if ! command -v clang >/dev/null; then
    skipped sanitizers "clang not installed"
  elif [ -d build-asan ] || CC=clang CXX=clang++ meson setup build-asan core \
    -Denable_cuda=false -Denable_sycl=false -Db_lto=false \
    -Db_sanitize=address,undefined -Db_lundef=false >/dev/null 2>&1; then
    if ninja -C build-asan >/tmp/preflight-asan.log 2>&1 &&
      meson test -C build-asan --suite=fast >>/tmp/preflight-asan.log 2>&1; then
      ok sanitizers
    else
      bad sanitizers
      grep -m3 -A4 'error:\|runtime error\|^Fail:' /tmp/preflight-asan.log | head -12
    fi
  else
    skipped sanitizers "meson setup failed"
  fi
fi

# ------------------------------------------------------------ clang-tidy --
if want tidy; then
  say "clang-tidy on changed files" "mirrors: Tidy Changed"
  if ! command -v clang-tidy >/dev/null; then
    skipped tidy "clang-tidy not installed"
  elif [ ! -f build/compile_commands.json ]; then
    skipped tidy "build/compile_commands.json missing — run the gcc stage first"
  else
    tidy_fail=0
    while IFS= read -r f; do
      [ -f "$f" ] || continue
      # Mirror the workflow's exclusion list: backends without a local toolchain.
      case "$f" in
        core/src/feature/arm64/* | core/src/cuda/* | core/src/feature/cuda/* | \
          core/src/sycl/* | core/src/feature/sycl/* | core/src/hip/* | \
          core/src/feature/hip/* | core/test/test_cuda_* | core/test/test_sycl* | \
          core/test/test_hip* | core/test/fuzz/* | core/src/mcp/* | \
          core/src/compat/win32/* | core/src/interop/pelorus_*) continue ;;
      esac
      n=$(clang-tidy -p build --quiet "$f" 2>/dev/null |
        grep -E 'warning:' | grep -vc 'clang-diagnostic')
      if [ "${n:-0}" -gt 0 ]; then
        printf '     %-52s %s warning(s)\n' "$f" "$n"
        tidy_fail=1
      fi
    done < <(changed_sources)
    if [ "$tidy_fail" -eq 0 ]; then
      ok tidy
    else
      bad tidy
    fi
  fi
fi

# --------------------------------------------------------------- cppcheck --
if want cppcheck; then
  say "cppcheck on changed files" "mirrors: Cppcheck"
  if ! command -v cppcheck >/dev/null; then
    skipped cppcheck "cppcheck not installed"
  else
    if changed_sources | xargs -r cppcheck --quiet --error-exitcode=1 \
      --suppress=missingInclude --suppress=missingIncludeSystem \
      -I core/include -I core/src 2>/tmp/preflight-cppcheck.log; then
      ok cppcheck
    else
      bad cppcheck
      head -8 /tmp/preflight-cppcheck.log | sed 's/^/     /'
    fi
  fi
fi

printf '\n\033[1m== preflight summary ==\033[0m  pass=%d fail=%d skip=%d\n' "$pass" "$fail" "$skip"
if [ "$fail" -gt 0 ]; then
  printf '   failed stages:%s\n' "$FAILED_STAGES"
  exit 1
fi
exit 0
