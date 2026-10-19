#!/usr/bin/env bash
# check-msvc-clz-shim.sh — guard the MSVC __builtin_clz shim against the
# Netflix/vmaf#1422 form that Netflix/vmaf#1551 retracts.
#
# MSVC's `__lzcnt` / `__lzcnt64` emit the LZCNT instruction (F3 0F BD)
# unconditionally, with no runtime feature gate. On an x86-64 without
# ABM/LZCNT the F3 prefix is ignored and the encoding retires as BSR, which
# returns the INDEX of the most-significant set bit instead of the
# leading-zero COUNT. Both scalar call sites of the shim
# (integer_vif.h::log2_32, integer_adm.c::get_best15_from32) then compute
# silently wrong VIF / ADM log2 shifts — no fault, no diagnostic, wrong VMAF.
# CI cannot catch it because every hosted Windows runner has LZCNT.
#
# The shim must therefore be written with `_BitScanReverse` /
# `_BitScanReverse64`, which are BSR by definition and present on every
# x86-64 part, and must carry an explicit architecture allowlist that
# enumerates every architecture MSVC targets -- ARM64 included. Per the MSVC
# intrinsics reference, `_BitScanReverse` is available on x86, ARM, x64 and
# ARM64, and only `_BitScanReverse64` is restricted (to x64 and ARM64), so no
# MSVC architecture needs to be excluded. An excluded one does not fall back to
# anything: the header is the sole definition of `__builtin_clz` for the
# generic scalar path, so that leg simply fails to compile.
#
# cl.exe has no `__builtin_clz` of its own either: a host C or C++ file that
# names it (directly or through a `#define`, as the host build of a device
# header does) must include the shim itself, or the Windows MSVC legs fail
# with C3861 "identifier not found" (test_adm_decouple_recip.cpp, #2095;
# T-ADM-DECOUPLE-RECIP-TEST-WINDOWS-2026-10-06). The SYCL and Metal sources
# are not compiled by cl.exe and are left out.
#
# Usage: scripts/ci/check-msvc-clz-shim.sh [repo-root]
# Exit 0 when the shim is in the required shape, 1 otherwise.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

ROOT="${1:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
HDR="$ROOT/core/src/feature/compat_builtin.h"

if [[ ! -f "$HDR" ]]; then
  echo "ERROR: $HDR not found" >&2
  exit 1
fi

rc=0

# (1) The intrinsic must not be *used*. The explanatory comment names it, so
#     only flag occurrences outside comment lines.
#
#     Matches the bare IDENTIFIER, not `__lzcnt(`: keying on the call syntax
#     let `#define LZ __lzcnt` + `LZ(x)` (or token pasting) reintroduce the
#     instruction while still passing this gate. There is no legitimate
#     non-comment mention of the identifier in this header, so requiring its
#     total absence outside comments is both stricter and simpler.
if grep -vE '^[[:space:]]*(\*|/\*|//)' "$HDR" | grep -qE '\b__lzcnt(64)?\b'; then
  echo "FAIL: $HDR calls __lzcnt/__lzcnt64." >&2
  echo "      LZCNT silently decodes as BSR on pre-Haswell x86-64 and yields" >&2
  echo "      wrong VIF/ADM shifts. Use _BitScanReverse. Netflix/vmaf#1551." >&2
  rc=1
fi

# (2) The BSR intrinsic must be present.
if ! grep -q '_BitScanReverse' "$HDR"; then
  echo "FAIL: $HDR does not use _BitScanReverse." >&2
  rc=1
fi

# (3) The MSVC guard must carry an explicit architecture allowlist covering
#     every architecture MSVC targets. Continuation lines are joined first,
#     because the guard legitimately spans several physical lines.
guard=$(sed -e :a -e '/\\$/N; s/\\\n//; ta' "$HDR" | grep -E '^#if defined\(_MSC_VER\)' || [ "$?" -eq 1 ])
if [[ -z "$guard" ]]; then
  echo "FAIL: $HDR has no '#if defined(_MSC_VER)' guard at all." >&2
  rc=1
elif ! grep -qE '_M_(X64|IX86)' <<<"$guard"; then
  echo "FAIL: the _MSC_VER guard in $HDR has no _M_X64 / _M_IX86 architecture test." >&2
  rc=1
elif ! grep -q '_M_ARM64' <<<"$guard"; then
  echo "FAIL: the _MSC_VER guard in $HDR does not cover _M_ARM64." >&2
  echo "      _BitScanReverse is available on x86, ARM, x64 and ARM64 (MSVC" >&2
  echo "      intrinsics reference); excluding ARM64 leaves __builtin_clz with" >&2
  echo "      no definition on the generic scalar path, so the leg cannot" >&2
  echo "      compile. Do not narrow this allowlist." >&2
  rc=1
fi

# (4) Nothing else in the tree may reintroduce the intrinsic either.
# Restricted to source extensions: rule (1) now matches the bare identifier,
# and documentation legitimately discusses `__lzcnt` in prose
# (core/src/feature/AGENTS.md explains why the shim must not use it). The rule
# is about CODE reintroducing the instruction.
# The audited shim is excluded by BASENAME, not by matching against "$HDR":
# grep -rl prints whatever path form it walked, which on MSYS / Git-for-Windows
# is not the same string as the absolute "$HDR" this script computes, so the
# previous `grep -vF "$HDR"` filter silently failed to exclude the shim there
# and the gate reported its own header (the Windows MinGW64 leg failed on
# exactly that). Rule (1) above already audits the shim itself.
others=$(grep -rlE '\b__lzcnt(64)?\b' "$ROOT/core/src" \
  --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' \
  --include='*.cu' --include='*.cuh' --include='*.hip' --include='*.mm' \
  --include='*.metal' --exclude='compat_builtin.h' 2>/dev/null || [ "$?" -eq 1 ])
if [[ -n "$others" ]]; then
  echo "FAIL: __lzcnt used outside the audited shim:" >&2
  echo "$others" >&2
  rc=1
fi

clz_users=$(grep -rlE '\b__builtin_clz(ll)?\b' "$ROOT/core/src" "$ROOT/core/test" \
  --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' \
  --exclude='compat_builtin.h' 2>/dev/null || [ "$?" -eq 1 ])
while IFS= read -r f; do
  case "$f" in
    "" | */sycl/* | */metal/*) continue ;;
  esac
  if grep -vE '^[[:space:]]*(\*|/\*|//)' "$f" | grep -qE '\b__builtin_clz(ll)?\b' &&
    ! grep -qE '^[[:space:]]*#[[:space:]]*include[[:space:]]+"(feature/)?compat_builtin\.h"' "$f"; then
    echo "FAIL: $f names __builtin_clz but does not include compat_builtin.h (MSVC C3861)." >&2
    rc=1
  fi
done <<<"$clz_users"

if [[ $rc -eq 0 ]]; then
  echo "OK: MSVC clz shim uses _BitScanReverse, is architecture-guarded and reaches every host user."
fi
exit $rc
