#!/usr/bin/env bash
# preflight.sh — run the CI checks that a gcc-only local build cannot catch.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# The repository's documented local gate (`make lint`, `make test-fast`) builds with
# ONE compiler. CI builds with several, and the difference is not academic: on
# 2026-09-07 a single branch shipped three separate portability breaks that were
# green locally and red in CI, each costing a full round-trip on a queue where
# one PR at a time can be in flight:
#
#   * a `static_assert` on `UINT_MAX <= SIZE_MAX/2/sizeof(ptr)` — true on LP64,
#     FALSE on 32-bit, so the i686 lane of the time would not compile the file
#     (that lane is retired: the fork is 64-bit only, ADR-1258);
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
set -euo pipefail
export LC_ALL=C

# The scanners next to this script, also when it checks another checkout.
PREFLIGHT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || exit 2
REPO_ROOT="$(git rev-parse --show-toplevel)" || exit 2
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

# Same selection as changed_sources, for CUDA device code. changed_sources lists
# C and C++ only, so the stage that checks `.cu` / `.cuh` constructs needs its
# own list (it used to filter changed_sources and therefore never saw a file).
changed_cuda() {
  if [ "$MODE" = full ]; then
    git ls-files '*.cu' '*.cuh'
    return
  fi
  {
    git diff --name-only --diff-filter=d "$BASE"...HEAD -- '*.cu' '*.cuh'
    git diff --name-only --diff-filter=d -- '*.cu' '*.cuh'
    git diff --name-only --diff-filter=d --cached -- '*.cu' '*.cuh'
    git ls-files --others --exclude-standard -- '*.cu' '*.cuh'
  } | sort -u
}

# Same selection as changed_sources, for Metal shaders. Kept separate because
# every other stage compiles C/C++ and would choke on a .metal path.
changed_metal() {
  if [ "$MODE" = full ]; then
    git ls-files '*.metal'
    return
  fi
  {
    git diff --name-only --diff-filter=d "$BASE"...HEAD -- '*.metal'
    git diff --name-only --diff-filter=d -- '*.metal'
    git diff --name-only --diff-filter=d --cached -- '*.metal'
    git ls-files --others --exclude-standard -- '*.metal'
  } | sort -u
}

# meson_assignment_has FILE VAR NEEDLE: the assignment `VAR = ...` of a meson
# file, with its backslash continuation lines, names NEEDLE. awk reads the
# whole file, so no reader closes the pipe early.
meson_assignment_has() {
  [ -f "$1" ] || return 1
  awk -v var="$2" -v needle="$3" '
    index($0, var " = ") && $0 ~ "^[[:space:]]*" var " = " { open = 1 }
    open {
      if (index($0, needle)) found = 1
      if ($0 !~ /\\[[:space:]]*$/) open = 0
    }
    END { exit found ? 0 : 1 }' "$1"
}

if [ "$MODE" = list ]; then
  cat <<'EOF'
stage          mirrors CI context            catches
-----          ------------------            -------
gcc            Ubuntu gcc(+DNN)              the baseline build
clang          Ubuntu clang(+DNN)            clang-only syntax, e.g. __attribute__(x)
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
  # A configured build has build.ninja; a bare build/ may only hold other
  # build trees (build/cuda, ...), and reusing it would fail at ninja.
  if [ -f build/build.ninja ] || CC=gcc CXX=g++ meson setup build core \
    -Denable_cuda=false -Denable_sycl=false -Db_lto=false >/dev/null 2>&1; then
    if ninja -C build >/tmp/preflight-gcc.log 2>&1 &&
      python3 scripts/ci/run_meson_test.py -- -C build --suite=fast \
        >>/tmp/preflight-gcc.log 2>&1; then
      ok gcc
    else
      bad gcc
      grep -m3 -A4 'FAILED:\|^Fail:' /tmp/preflight-gcc.log | head -12 ||
        echo "     (no matching lines in /tmp/preflight-gcc.log)"
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
      python3 scripts/ci/run_meson_test.py -- -C build-clang --suite=fast \
        >>/tmp/preflight-clang.log 2>&1; then
      ok clang
    else
      bad clang
      grep -m3 -A4 'error:\|^Fail:' /tmp/preflight-clang.log | head -12 ||
        echo "     (no matching lines in /tmp/preflight-clang.log)"
    fi
  else
    skipped clang "meson setup failed"
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
    hits=$(changed_sources | xargs -r grep -nE "$pat" 2>/dev/null | awk 'NR <= 5') || hits="" # grep / xargs exit non-zero when nothing matches
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

  # nvcc's MSVC host frontend rejects C++20 designated initializers in device
  # code -- "error: expected an expression" at the first `.field =` -- while its
  # GCC frontend accepts them, so the construct passes every Linux build and
  # only the Windows CUDA lane sees it (2026-09-20, adm_cm.cu, bug ledger L-84).
  # Device code uses positional aggregate initialization with the field name in
  # a trailing comment instead.
  cu_designated=$(changed_cuda | while read -r f; do
    grep -nE '(\{|,)[[:space:]]*\.[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=|^[[:space:]]*\.[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=.*(,|\{)[[:space:]]*$' "$f" 2>/dev/null |
      grep -vE '^[0-9]+:[[:space:]]*(\*|/\*|//)' | sed "s|^|$f:|"
  done | awk 'NR <= 5') || cu_designated=""
  if [ -n "$cu_designated" ]; then
    printf '     %s\n' 'designated initializer in CUDA device code — nvcc/MSVC "expected an expression"'
    printf '%s\n' "$cu_designated" | sed 's/^/       /'
    msvc_fail=1
  fi

  # ADR-1138: C translation units keep `NULL`. MSVC's documented /std:clatest
  # C23 feature set does not implement the `nullptr` keyword, and the Windows
  # lane compiles core/ with cl.exe. gcc accepts it and clang accepts it under
  # -std=c23, so nothing local objects. Comment lines are skipped because the
  # ADR-1138 NOLINT bracket names the keyword in prose.
  c_nullptr=$(changed_sources | grep -E '\.c$' | while read -r f; do
    grep -nE '\bnullptr\b' "$f" 2>/dev/null |
      grep -vE '^[0-9]+:[[:space:]]*(\*|/\*|//)' | sed "s|^|$f:|"
  done | awk 'NR <= 60') || c_nullptr=""
  if [ -n "$c_nullptr" ]; then
    printf '     %s\n' 'nullptr in a C translation unit — MSVC C2065; ADR-1138 keeps C on NULL'
    printf '%s\n' "$c_nullptr" | sed 's/^/       /'
    msvc_fail=1
  fi

  # <windows.h> defines min() and max() as function-like macros unless NOMINMAX
  # is set, and the SYCL headers pull it in: `std::numeric_limits<T>::max()`
  # then fails with "too few arguments provided to function-like macro
  # invocation" on the Windows MSVC+SYCL lane only (2026-10-02,
  # sycl_exact_fp.h). The parenthesised form `(std::numeric_limits<T>::max)()`
  # compiles everywhere.
  sycl_minmax=$(changed_sources | grep -E '^core/src/(feature/)?sycl/' | while read -r f; do
    grep -nE 'numeric_limits<[^>]+>::(max|min)\(\)' "$f" 2>/dev/null |
      grep -vE '\(std::numeric_limits<[^>]+>::(max|min)\)\(\)' |
      grep -vE '^[0-9]+:[[:space:]]*(\*|/\*|//)' | sed "s|^|$f:|"
  done | awk 'NR <= 60') || sycl_minmax=""
  if [ -n "$sycl_minmax" ]; then
    printf '     %s\n' 'numeric_limits<T>::max() / min() in a SYCL source — <windows.h> macro clash; write (std::numeric_limits<T>::max)()'
    printf '%s\n' "$sycl_minmax" | sed 's/^/       /'
    msvc_fail=1
  fi

  # Meson runs the Python contract tests on Windows too. `str(path.relative_to(root))`
  # yields backslashes there, so a dictionary keyed by it and read with a
  # 'dir/file' literal raises KeyError on the Windows lanes only (2026-10-02,
  # two tests). `path.relative_to(root).as_posix()` is the portable key.
  if [ "$MODE" = full ]; then
    py_tests=$(git ls-files 'core/test/*.py')
  else
    py_tests=$({
      git diff --name-only --diff-filter=d "$BASE"...HEAD -- 'core/test/*.py'
      git diff --name-only --diff-filter=d -- 'core/test/*.py'
      git diff --name-only --diff-filter=d --cached -- 'core/test/*.py'
      git ls-files --others --exclude-standard -- 'core/test/*.py'
    } | sort -u)
  fi
  py_keys=$(printf '%s\n' "$py_tests" | grep -v '^$' | while read -r f; do
    grep -nE '\[str\([A-Za-z_]+\.relative_to\([^]]*\)\)\][[:space:]]*=|^[[:space:]]*str\([A-Za-z_]+\.relative_to\([^)]*\)\):' "$f" 2>/dev/null | sed "s|^|$f:|"
  done | awk 'NR <= 60') || py_keys=""
  if [ -n "$py_keys" ]; then
    printf '     %s\n' 'dictionary keyed by str(path.relative_to(...)) in a core/test Python test — backslashes on Windows; use .as_posix()'
    printf '%s\n' "$py_keys" | sed 's/^/       /'
    msvc_fail=1
  fi

  # Metal Shading Language predefines `half` (16-bit float) and the vector type
  # names, so a `.metal` file that declares a variable with one of those names
  # is a redeclaration -- "cannot combine with previous 'type-name' declaration
  # specifier", plus a parse error at every later use. The C / CUDA / HIP twins
  # these kernels are ported from have no such reservation, which is how the
  # name travels. The macOS Metal lanes are not in the required set, so this is
  # otherwise only caught after merge.
  msl_reserved='(half|float2|float3|float4|int2|int3|int4|uint2|uint3|uint4|bool2|bool3|bool4|sampler)'
  msl_hits=$(changed_metal | while read -r f; do
    grep -nE "\b(ulong|uint|int|float|bool|long|short|auto|char) +$msl_reserved\b" "$f" 2>/dev/null |
      awk 'NR <= 2' | sed "s|^|$f:|"
  done | awk 'NR <= 5') || msl_hits=""
  if [ -n "$msl_hits" ]; then
    printf '     %s\n' 'MSL reserved type name used as a variable — macOS Metal redeclaration error'
    printf '%s\n' "$msl_hits" | sed 's/^/       /'
    msvc_fail=1
  fi

  # `M_PI` and friends are POSIX/X-Open, not ISO C. glibc exposes them only
  # under __USE_MISC/__USE_XOPEN, which `-std=c23` disables by defining
  # __STRICT_ANSI__; the Linux lanes see them because meson passes
  # -D_GNU_SOURCE. The MSVC runtime defines them only under
  # _USE_MATH_DEFINES, and MinGW-w64's <math.h> hides them under
  # __STRICT_ANSI__ unless it is defined. Since Netflix/vmaf 4e150067b (Q-301)
  # core/meson.build passes -D_USE_MATH_DEFINES as a project argument on
  # Windows hosts, and no translation unit keeps a guard or a copy of the
  # constants. icpx compiles the SYCL translation units in custom targets,
  # which project arguments never reach, so the define is project-wide only
  # when core/src/meson.build adds the same list to both SYCL argument lists
  # (sycl_common_args, sycl_feature_tail_args): without that the Windows SYCL
  # build failed on M_PI (T-SYCL-WINDOWS-M-PI-UNDECLARED-2026-10-09). The macros
  # are accepted while all three are in the build files; without them, a file
  # needs the old per-file guard (_USE_MATH_DEFINES before <math.h>, or an
  # `#ifndef M_PI` fallback, in the file or in an in-tree header it includes).
  m_macros='\bM_(PI|E|SQRT2|LN2|LN10|PI_2|PI_4|1_PI|2_PI|SQRT1_2|LOG2E|LOG10E)\b'
  math_list_define="^[[:space:]]*vmaf_math_constant_args = \['-D_USE_MATH_DEFINES'\]"
  math_project_define="^[[:space:]]*add_project_arguments\(vmaf_math_constant_args,[^)]*language: *\[[^]]*'c'"
  math_defines_project_wide=0
  if [ -f core/meson.build ] && grep -qE "$math_list_define" core/meson.build &&
    grep -qE "$math_project_define" core/meson.build &&
    meson_assignment_has core/src/meson.build sycl_common_args vmaf_math_constant_args &&
    meson_assignment_has core/src/meson.build sycl_feature_tail_args vmaf_math_constant_args; then
    math_defines_project_wide=1
  fi
  c_math=""
  if [ "$math_defines_project_wide" -eq 0 ]; then
    # The probes of this stage trim with awk, which reads its whole input:
    # `head` would close the pipe while a scan still writes, the scan would
    # die of SIGPIPE, and under pipefail the `|| x=""` fallback would throw
    # the findings away (a five-finding diff passed this stage locally).
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
      grep -nE "$m_macros" "$f" | awk 'NR <= 2' | sed "s|^|$f:|"
    done | awk 'NR <= 5') || c_math=""
  fi
  if [ -n "$c_math" ]; then
    printf '     %s\n' 'M_* math macro without a _USE_MATH_DEFINES define — MSVC / MinGW64 C2065'
    printf '     %s\n' "  (core/meson.build has no add_project_arguments('-D_USE_MATH_DEFINES', ...)"
    printf '     %s\n' '   for Windows hosts, and the file has no per-file guard)'
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
    xargs -r python3 "$REPO_ROOT/scripts/dev/find-nonconst-static-init.py" 2>/dev/null | awk 'NR <= 5') || c_static_init=""
  if [ -n "$c_static_init" ]; then
    printf '     %s\n' 'non-constant initialiser in a static aggregate — MSVC C2099; use #define'
    printf '%s\n' "$c_static_init" | sed 's/^/       /'
    msvc_fail=1
  fi
  # MSVC ships no <unistd.h>, <dlfcn.h>, <poll.h>, <sys/socket.h> and the rest
  # of the POSIX-only headers, so a source the Windows build compiles must
  # include one only under a platform conditional. Every Linux and macOS lane
  # accepts the unguarded include; only the required Windows MSVC lanes, which
  # neither the local gates nor the merge train build, refuse it (2026-10-08:
  # cuda/import_vulkan.c and <unistd.h>). Sources a meson.build keeps off
  # Windows are skipped; a file that cannot be either is a named exception
  # (.config/lint-exceptions.d/msvcism-posix-headers.toml). A scan that cannot
  # run fails the stage: it must never pass for want of a result.
  posix_files=$({
    changed_sources
    changed_cuda
  } | sort -u)
  posix_hits=""
  if [ -n "$posix_files" ]; then
    if ! posix_kept=$(printf '%s\n' "$posix_files" |
      xargs python3 "$PREFLIGHT_DIR/../ci/lint_exceptions.py" filter msvcism-posix-headers --) ||
      ! posix_hits=$(printf '%s\n' "$posix_kept" |
        xargs -r python3 "$PREFLIGHT_DIR/find-posix-only-headers.py"); then
      printf '     %s\n' 'POSIX-only header scan did not run (scripts/dev/find-posix-only-headers.py)'
      msvc_fail=1
    fi
  fi
  if [ -n "$posix_hits" ]; then
    printf '     %s\n' 'POSIX-only header outside a platform conditional in a source the Windows build compiles — MSVC C1083'
    printf '     %s\n' '  (guard it with #ifndef _WIN32 / #ifdef __linux__, use compat/crt_portable.h, or keep the'
    printf '     %s\n' '   file off Windows in its meson.build: host_machine.system() != '"'"'windows'"'"')'
    printf '%s\n' "$posix_hits" | head -20 | sed 's/^/       /'
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
      python3 scripts/ci/run_meson_test.py -- -C build-asan --suite=fast \
        >>/tmp/preflight-asan.log 2>&1; then
      ok sanitizers
    else
      bad sanitizers
      grep -m3 -A4 'error:\|runtime error\|^Fail:' /tmp/preflight-asan.log | head -12 ||
        echo "     (no matching lines in /tmp/preflight-asan.log)"
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
          core/src/compat/win32/*) continue ;;
      esac
      n=$(clang-tidy -p build --quiet "$f" 2>/dev/null |
        grep -E 'warning:' | grep -vc 'clang-diagnostic') || n="${n:-0}" # grep -c exits 1 when it counts 0
      if [ "${n:-0}" -gt 0 ]; then
        printf '     %-52s %s warning(s)\n' "$f" "$n"
        tidy_fail=1
      fi
    done < <(changed_sources | python3 scripts/ci/pelorus_mirror.py filter)
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
