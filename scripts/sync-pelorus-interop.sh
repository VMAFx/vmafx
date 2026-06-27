#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# sync-pelorus-interop.sh — re-sync / drift-guard for the vendored Pelorus
# <-> vmafx interop ABI (ADR-1113).
#
# The data-plane interop ABI is SINGLE-SOURCED in pelorus (ADR-0103). vmafx
# carries a verbatim, append-only mirror under:
#
#   core/include/libvmaf/pelorus/{pelorus,interop,deband}.h
#   core/src/interop/pelorus_{interop,deband_params,version}.c
#   core/test/test_pelorus_interop.c   (conformance fixture)
#
# Each vendored file is byte-identical to its pelorus origin EXCEPT for two
# documented local edits:
#   1. a "VENDORED FROM ... DO NOT EDIT" banner inserted after the license
#      header (and, for the test, a Lusoris-authored header + banner), and
#   2. intra-pelorus includes rewritten from "pelorus/<x>.h" to
#      "libvmaf/pelorus/<x>.h" so they resolve under core/include/.
#
# This script strips those two known deltas and diffs the remainder against a
# pelorus checkout pinned at PELORUS_VENDOR_SHA. Any other difference is DRIFT
# and fails the run (exit 1) — the vendored mirror must never diverge silently.
#
# Modes:
#   (default)   check for drift; exit 1 if the mirror differs from pelorus.
#   --update    rewrite the vendored copies from the pelorus checkout (re-vendor
#               after a deliberate pelorus ABI-minor bump). Re-run without
#               --update afterwards to confirm clean.
#
# Usage:
#   scripts/sync-pelorus-interop.sh [--update] [PELORUS_CHECKOUT]
#   PELORUS_CHECKOUT defaults to $PELORUS_DIR or ../pelorus relative to repo root.
#
# Exit 0 = no drift (or --update succeeded). Exit 1 = drift / error.

set -euo pipefail

# --- Pinned source of truth ------------------------------------------------
# The pelorus commit this mirror was vendored from. Bump this (and re-vendor
# via --update) only on a deliberate pelorus interop ABI change. Keep in lock
# step with the banner SHA in every vendored file and docs/api/pelorus-interop.md.
PELORUS_VENDOR_SHA="818d844"

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

mode="check"
pelorus_dir=""
for arg in "$@"; do
  case "$arg" in
    --update) mode="update" ;;
    --help | -h)
      sed -n '4,40p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    -*)
      printf 'unknown flag: %s\n' "$arg" >&2
      exit 64
      ;;
    *) pelorus_dir="$arg" ;;
  esac
done

if [ -z "$pelorus_dir" ]; then
  pelorus_dir="${PELORUS_DIR:-$repo_root/../pelorus}"
fi

if [ ! -d "$pelorus_dir/libpelorus" ]; then
  printf 'error: pelorus checkout not found at %s\n' "$pelorus_dir" >&2
  printf '       pass the path explicitly or set PELORUS_DIR.\n' >&2
  exit 1
fi

src_root="$pelorus_dir/libpelorus"

# --- Pin resolution --------------------------------------------------------
# The mirror is pinned to PELORUS_VENDOR_SHA, NOT to whatever the local
# checkout's HEAD currently is. If the checkout is a git repo that knows the
# pinned commit, read the vendored sources from that exact tree object
# (`git show <SHA>:<path>`) so the guard stays pin-accurate even when the
# working tree has advanced past the pin. Only when git extraction is
# impossible (not a repo, or the SHA is unknown) do we fall back to the
# working-tree files, with a loud warning.
use_git_pin=0
if git -C "$pelorus_dir" rev-parse --short HEAD >/dev/null 2>&1; then
  if git -C "$pelorus_dir" cat-file -e "${PELORUS_VENDOR_SHA}^{commit}" 2>/dev/null; then
    use_git_pin=1
    have_sha="$(git -C "$pelorus_dir" rev-parse --short HEAD)"
    case "$have_sha" in
      "$PELORUS_VENDOR_SHA"*) : ;;
      *)
        printf 'note: pelorus checkout HEAD is %s; reading vendored sources from pinned %s.\n' \
          "$have_sha" "$PELORUS_VENDOR_SHA" >&2
        ;;
    esac
  else
    printf 'WARNING: pinned commit %s not found in %s; diffing against the\n' \
      "$PELORUS_VENDOR_SHA" "$pelorus_dir" >&2
    printf '         working tree instead (fetch the pin for a faithful check).\n' >&2
  fi
else
  printf 'WARNING: %s is not a git checkout; diffing against its files as-is.\n' \
    "$pelorus_dir" >&2
fi

# Emit the pinned (or working-tree fallback) content of a pelorus-relative path.
read_src() {
  local rel="$1"
  if [ "$use_git_pin" -eq 1 ]; then
    git -C "$pelorus_dir" show "${PELORUS_VENDOR_SHA}:libpelorus/$rel"
  else
    cat "$src_root/$rel"
  fi
}

# --- Vendored file manifest ------------------------------------------------
# Each row: "<pelorus-relative-source>|<vmafx-relative-dest>|<inc-from>|<inc-to>"
# inc-from/inc-to may be empty (no include to rewrite).
manifest=(
  "include/pelorus/pelorus.h|core/include/libvmaf/pelorus/pelorus.h||"
  "include/pelorus/interop.h|core/include/libvmaf/pelorus/interop.h|pelorus/pelorus.h|libvmaf/pelorus/pelorus.h"
  "include/pelorus/deband.h|core/include/libvmaf/pelorus/deband.h|pelorus/pelorus.h|libvmaf/pelorus/pelorus.h"
  "include/pelorus/denoise.h|core/include/libvmaf/pelorus/denoise.h|pelorus/pelorus.h|libvmaf/pelorus/pelorus.h"
  "src/interop.c|core/src/interop/pelorus_interop.c|pelorus/interop.h|libvmaf/pelorus/interop.h"
  "src/deband_params.c|core/src/interop/pelorus_deband_params.c|pelorus/deband.h|libvmaf/pelorus/deband.h"
  "src/denoise_params.c|core/src/interop/pelorus_denoise_params.c|pelorus/denoise.h|libvmaf/pelorus/denoise.h"
  "src/qp_report_csv.c|core/src/interop/pelorus_qp_report_csv.c|pelorus/interop.h|libvmaf/pelorus/interop.h"
  "src/version.c|core/src/interop/pelorus_version.c|pelorus/pelorus.h|libvmaf/pelorus/pelorus.h"
)

# The test fixture is vendored with a Lusoris-authored header rather than a
# pelorus license clone, so it is diffed body-only (from the first #include).
test_dst="$repo_root/core/test/test_pelorus_interop.c"

# --- Helpers ---------------------------------------------------------------

# Print a vendored file with the "VENDORED FROM" banner block + its trailing
# blank line removed, so the remainder can be compared to the pelorus origin.
strip_banner() {
  awk '
    BEGIN { inblk = 0; isban = 0; skipblank = 0 }
    skipblank == 1 { if ($0 == "") { skipblank = 0; next } skipblank = 0 }
    /^\/\*$/ && inblk == 0 { buf = $0 "\n"; inblk = 1; isban = 0; next }
    inblk == 1 {
      buf = buf $0 "\n"
      if (index($0, "VENDORED FROM") > 0) { isban = 1 }
      if ($0 ~ /^ \*\/$/) {
        if (isban == 1) { inblk = 0; buf = ""; skipblank = 1; next }
        else { printf "%s", buf; inblk = 0; buf = ""; next }
      }
      next
    }
    { print }
  ' "$1"
}

# Re-create one vendored file from the pinned pelorus source (read on stdin):
# license header (lines 1-17) + DO-NOT-EDIT banner + body, with the intra-
# pelorus include rewritten.
emit() {
  local rel_src="$1" dst="$2" inc_from="$3" inc_to="$4"
  local pinned
  pinned="$(read_src "$rel_src")"
  {
    printf '%s\n' "$pinned" | head -n 17
    echo
    echo '/*'
    echo ' * VENDORED FROM VMAFx/pelorus@'"$PELORUS_VENDOR_SHA"' — DO NOT EDIT. Append-only ABI; single'
    echo ' * source of truth is pelorus. Re-sync via scripts/sync-pelorus-interop.sh.'
    echo ' * See docs/adr/1113-vendor-pelorus-interop-abi.md.'
    if [ -n "$inc_from" ]; then
      echo ' *'
      echo ' * Local edit vs the pelorus original: the intra-pelorus #include below is'
      echo ' * rewritten from "'"$inc_from"'" to "'"$inc_to"'" so it resolves'
      echo ' * under core/include/. Nothing else is changed.'
    fi
    echo ' */'
    echo
    printf '%s\n' "$pinned" | tail -n +19
  } >"$dst.tmp"
  if [ -n "$inc_from" ]; then
    sed -i 's|#include "'"$inc_from"'"|#include "'"$inc_to"'"|' "$dst.tmp"
  fi
  mv "$dst.tmp" "$dst"
}

drift=0

for row in "${manifest[@]}"; do
  IFS='|' read -r rel_src rel_dst inc_from inc_to <<<"$row"
  dst="$repo_root/$rel_dst"

  if [ "$mode" = "update" ]; then
    emit "$rel_src" "$dst" "$inc_from" "$inc_to"
    printf 're-vendored: %s\n' "$rel_dst"
    continue
  fi

  if [ ! -f "$dst" ]; then
    printf 'DRIFT: vendored file missing: %s\n' "$rel_dst" >&2
    drift=1
    continue
  fi

  # Compare the de-bannered vendored body against the pinned pelorus origin with
  # the documented include rewrite applied.
  if [ -n "$inc_from" ]; then
    expected="$(read_src "$rel_src" | sed 's|#include "'"$inc_from"'"|#include "'"$inc_to"'"|')"
  else
    expected="$(read_src "$rel_src")"
  fi
  actual="$(strip_banner "$dst")"

  if [ "$expected" != "$actual" ]; then
    printf 'DRIFT: %s differs from pinned pelorus %s\n' "$rel_dst" "$rel_src" >&2
    diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") >&2 || true
    drift=1
  fi
done

# --- Conformance fixture --------------------------------------------------
# The fixture is split into two parts:
#   header = the vmafx Lusoris-authored part BEFORE the first vendored include
#            (license header + a vmafx-specific doc block); NOT a pelorus clone,
#            so it is preserved verbatim across re-vendors.
#   body   = from the first vendored include onward; the verbatim pelorus
#            test/interop_test.c body with "pelorus/" -> "libvmaf/pelorus/".
# The pelorus repo formats its C with the same .clang-format as vmafx (its
# config notes it "matches the vmafx sibling"), so the re-vendored body is
# already clang-format clean — we vendor it raw and do NOT reformat it (the
# drift check below is whitespace-insensitive / token equality, so a stray
# reformat would also pass it, but keeping it raw keeps the body a faithful
# mirror of the pelorus source token-for-token).

# Emit the rewritten pelorus body (first vendored include onward) on stdout.
# The `f` latch is SET BEFORE the print test so the triggering include line is
# emitted exactly once (a `f{print}` rule ahead of the trigger rule would print
# every include after the first one twice).
fixture_body_pel() {
  read_src "test/interop_test.c" |
    awk '/^#include "pelorus\// { f = 1 } f { print }' |
    sed 's|#include "pelorus/|#include "libvmaf/pelorus/|'
}

if [ "$mode" = "update" ]; then
  # --update re-vendors the manifest files above; it MUST also re-vendor the
  # fixture body, or the immediately-following drift check fails on a fixture
  # that still carries the previous pin's body. Preserve the Lusoris-authored
  # header (everything before the first "libvmaf/pelorus/" include) and replace
  # the body with the freshly-pinned pelorus body.
  if [ ! -f "$test_dst" ]; then
    printf 'error: conformance fixture missing: core/test/test_pelorus_interop.c\n' >&2
    printf '       (cannot re-vendor body without the Lusoris-authored header)\n' >&2
    exit 1
  fi
  {
    # Lusoris-authored header: up to (but not including) the first vendored
    # include. sed '/.../q' prints through the matched line; drop that line
    # with `head -n -1` so the body's own include leads the body section.
    sed '/^#include "libvmaf\/pelorus\//q' "$test_dst" | head -n -1
    fixture_body_pel
  } >"$test_dst.tmp"
  mv "$test_dst.tmp" "$test_dst"
  printf 're-vendored: core/test/test_pelorus_interop.c (body)\n'
fi

if [ "$mode" = "check" ]; then
  if [ ! -f "$test_dst" ]; then
    printf 'DRIFT: conformance fixture missing: core/test/test_pelorus_interop.c\n' >&2
    drift=1
  else
    body_pel="$(fixture_body_pel)"
    body_vmafx="$(awk '/^#include "libvmaf\/pelorus\// { f = 1 } f { print }' "$test_dst")"
    if [ "$(printf '%s' "$body_pel" | tr -d '[:space:]')" \
      != "$(printf '%s' "$body_vmafx" | tr -d '[:space:]')" ]; then
      printf 'DRIFT: conformance fixture body differs from pelorus test/interop_test.c\n' >&2
      drift=1
    fi
  fi
fi

# --- Record the synced ABI minor ------------------------------------------
abi_minor="$(grep -E '#define[[:space:]]+PELORUS_ABI_MINOR' \
  "$repo_root/core/include/libvmaf/pelorus/interop.h" 2>/dev/null |
  grep -oE '[0-9]+u?' | head -n1 | tr -d 'u')"
abi_major="$(grep -E '#define[[:space:]]+PELORUS_ABI_MAJOR' \
  "$repo_root/core/include/libvmaf/pelorus/interop.h" 2>/dev/null |
  grep -oE '[0-9]+u?' | head -n1 | tr -d 'u')"

if [ "$mode" = "update" ]; then
  printf '\nre-vendor complete from pelorus@%s (ABI %s.%s).\n' \
    "$PELORUS_VENDOR_SHA" "${abi_major:-?}" "${abi_minor:-?}"
  printf 'Re-run without --update to confirm no drift, then rebuild + run\n'
  printf '  meson test -C core/build-cpu test_pelorus_interop\n'
  exit 0
fi

if [ "$drift" -ne 0 ]; then
  printf '\nFAIL: vendored Pelorus interop ABI has drifted from pelorus@%s.\n' \
    "$PELORUS_VENDOR_SHA" >&2
  printf '      Re-sync deliberately with: %s --update %s\n' "$0" "$pelorus_dir" >&2
  exit 1
fi

printf 'OK: vendored Pelorus interop ABI matches pelorus@%s (ABI %s.%s, minor=%s).\n' \
  "$PELORUS_VENDOR_SHA" "${abi_major:-?}" "${abi_minor:-?}" "${abi_minor:-?}"
exit 0
