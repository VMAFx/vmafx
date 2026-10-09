#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# sync-pelorus-interop.sh — re-sync / drift-guard for the vendored Pelorus
# <-> vmafx interop ABI (ADR-1113).
#
# The data-plane interop ABI is SINGLE-SOURCED in pelorus (ADR-0103). vmafx
# carries a verbatim, append-only mirror under:
#
#   core/include/libvmaf/pelorus/{pelorus,interop,deband,denoise}.h
#   core/src/interop/pelorus_{interop,deband_params,denoise_params,
#                            qp_report_csv,version}.c
#   core/test/test_pelorus_interop.c   (conformance fixture)
#
# Each vendored file is byte-identical to its pelorus origin EXCEPT for two
# documented local edits:
#   1. a "VENDORED FROM ... DO NOT EDIT" banner inserted after the license
#      header (and, for the test, a Lusoris-authored header + banner), and
#   2. intra-pelorus includes rewritten from "pelorus/<x>.h" to
#      "libvmaf/pelorus/<x>.h" so they resolve under core/include/.
#
# Every pinned source must carry exactly the SPDX line PELORUS_MIRROR_LICENSE
# names in its license header. A Pelorus licence change therefore fails both
# modes until a reviewed VMAFx change updates this value together with
# REUSE.toml and docs/credits.yaml.
#
# This script re-renders those two known deltas from a pelorus checkout pinned
# at PELORUS_VENDOR_SHA and compares the resulting file bytes through EOF. Any
# other difference is DRIFT and fails the run (exit 1) — the vendored mirror
# must never diverge silently.
#
# Modes:
#   (default)   check for drift; exit 1 if the mirror differs from pelorus.
#   --update    rewrite the vendored copies from the pelorus checkout (re-vendor
#               after a reviewed ABI addition or parser correctness/security
#               release). Re-run without --update afterwards to confirm clean.
#
# Usage:
#   scripts/sync-pelorus-interop.sh [--update] [PELORUS_CHECKOUT]
#   PELORUS_CHECKOUT defaults to $PELORUS_DIR or ../pelorus relative to repo root.
#
# Exit 0 = no drift (or --update succeeded). Exit 1 = drift / error.

set -euo pipefail

# --- Pinned source of truth ------------------------------------------------
# The exact released Pelorus commit this mirror was vendored from (tag v0.3.0).
# Re-pin for a reviewed interop ABI addition or a parser correctness/security
# fix, even when ABI major/minor stay unchanged. Keep this in lock step with
# every vendored banner and docs/api/pelorus-interop.md.
PELORUS_VENDOR_SHA="e2e4040311a443210927549c3a336f909c6473f3"
# The licence every pinned source declares (Pelorus ADR-0171, VMAFx ADR-1250).
PELORUS_MIRROR_LICENSE="EUPL-1.2"

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
mirror_policy="$repo_root/scripts/ci/pelorus_mirror.py"

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

# --- Pin resolution --------------------------------------------------------
# The mirror is pinned to PELORUS_VENDOR_SHA, NOT to whatever the local
# checkout's HEAD currently is. If the checkout is a git repo that knows the
# pinned commit, read the vendored sources from that exact tree object
# (`git show <SHA>:<path>`) so the guard stays pin-accurate even when HEAD has
# advanced. A working-tree fallback could bless unreviewed or locally modified
# bytes, so absence of Git provenance or the pinned object fails closed.
if ! git -C "$pelorus_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  printf 'error: pelorus source must be a Git checkout: %s\n' "$pelorus_dir" >&2
  exit 1
fi
if ! git -C "$pelorus_dir" cat-file -e "${PELORUS_VENDOR_SHA}^{commit}" 2>/dev/null; then
  printf 'error: pinned Pelorus commit is unavailable: %s\n' "$PELORUS_VENDOR_SHA" >&2
  printf '       fetch that object into %s before checking the mirror.\n' "$pelorus_dir" >&2
  exit 1
fi
have_sha="$(git -C "$pelorus_dir" rev-parse --short HEAD)"
case "$(git -C "$pelorus_dir" rev-parse HEAD)" in
  "$PELORUS_VENDOR_SHA") : ;;
  *)
    printf 'note: pelorus checkout HEAD is %s; reading vendored sources from pinned %s.\n' \
      "$have_sha" "$PELORUS_VENDOR_SHA" >&2
    ;;
esac

_SYNC_TMPDIRS=()
sync_tmp="$(mktemp -d "${TMPDIR:-/tmp}/vmafx-pelorus-sync.XXXXXX")"
_SYNC_TMPDIRS+=("$sync_tmp")
_cleanup() {
  local tmp_dir
  for tmp_dir in "${_SYNC_TMPDIRS[@]+"${_SYNC_TMPDIRS[@]}"}"; do
    rm -rf -- "$tmp_dir"
  done
}
trap '_cleanup' EXIT INT TERM

# Emit the pinned content of a pelorus-relative path.
read_src() {
  local rel="$1"
  git -C "$pelorus_dir" show "${PELORUS_VENDOR_SHA}:libpelorus/$rel"
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

# The conformance fixture uses a canonical VMAFx-authored prefix followed by
# the transformed Pelorus body. Both parts are rendered and checked as one file.
test_rel="core/test/test_pelorus_interop.c"
test_dst="$repo_root/$test_rel"

# --- Helpers ---------------------------------------------------------------

# Render one canonical vendored file from the pinned Pelorus source:
# license header (the leading comment block, which must name
# PELORUS_MIRROR_LICENSE in its one SPDX line) + DO-NOT-EDIT banner + body, with
# the intra-pelorus include rewritten. Python operates on bytes so a missing or
# extra EOF newline cannot be erased by shell command substitution or a line
# printer.
render_vendor() {
  local rel_src="$1" inc_from="$2" inc_to="$3"
  read_src "$rel_src" | python3 -c '
import sys

sha, include_from, include_to, license_id = sys.argv[1:]
# Split so REUSE does not take the joined tag for the licence of this script.
tag = ("SPDX-" + "License-Identifier:").encode()
source = sys.stdin.buffer.read()
lines = source.splitlines(keepends=True)
end = lines.index(b" */\n") if b" */\n" in lines else -1
if end < 1 or lines[0] != b"/**\n" or end + 1 >= len(lines) or lines[end + 1] != b"\n":
    raise SystemExit("unexpected Pelorus license-header layout")
spdx = [line for line in lines[:end] if tag in line]
if spdx != [b" * " + tag + f" {license_id}\n".encode()]:
    raise SystemExit(f"Pelorus license header does not declare exactly {license_id}: {spdx}")
body = b"".join(lines[end + 2:])
local_edit = ""
if include_from:
    old = f"#include \"{include_from}\"".encode()
    new = f"#include \"{include_to}\"".encode()
    if body.count(old) != 1:
        raise SystemExit(f"expected one include to rewrite: {include_from}")
    body = body.replace(old, new)
    local_edit = (
        " *\n"
        " * Local edit vs the pelorus original: the intra-pelorus #include below is\n"
        f" * rewritten from \"{include_from}\" to \"{include_to}\" so it resolves\n"
        " * under core/include/. Nothing else is changed.\n"
    )
banner = (
    "\n/*\n"
    f" * VENDORED FROM VMAFx/pelorus@{sha} — DO NOT EDIT.\n"
    " * Append-only ABI; single\n"
    " * source of truth is pelorus. Re-sync via scripts/sync-pelorus-interop.sh.\n"
    " * See docs/adr/1113-vendor-pelorus-interop-abi.md.\n"
    f"{local_edit}"
    " */\n\n"
).encode()
sys.stdout.buffer.write(b"".join(lines[:end + 1]) + banner + body)
' "$PELORUS_VENDOR_SHA" "$inc_from" "$inc_to" "$PELORUS_MIRROR_LICENSE"
}

drift=0

# --update renders every file here first and moves them into the tree only when
# all of them rendered, so a refused source (wrong licence, unexpected layout)
# leaves the mirror as it was.
staged_path() {
  printf '%s/%s.staged' "$sync_tmp" "$(printf '%s' "$1" | tr '/' '_')"
}

# The lint carve-out is valid only for files the shared path manifest owns.
# First prove this script's render destinations match that manifest, then
# reject any added, removed, or renamed tracked path in the mirror namespaces.
# The source glob deliberately has no suffix restriction: a future .cc/.cpp or
# header variant cannot silently inherit a prefix-based exemption. `git
# ls-files` deliberately ignores untracked developer artifacts.
check_render_manifest() {
  local row rel_src rel_dst inc_from inc_to
  local rendered_unsorted="$sync_tmp/rendered-mirrors.unsorted"
  local rendered="$sync_tmp/rendered-mirrors"
  local shared="$sync_tmp/shared-mirrors"

  : >"$rendered_unsorted"
  for row in "${manifest[@]}"; do
    IFS='|' read -r rel_src rel_dst inc_from inc_to <<<"$row"
    printf '%s\n' "$rel_dst" >>"$rendered_unsorted"
  done
  printf '%s\n' "$test_rel" >>"$rendered_unsorted"

  if ! python3 "$mirror_policy" list >"$shared"; then
    printf 'error: cannot read shared Pelorus mirror manifest\n' >&2
    return 1
  fi
  LC_ALL=C sort "$rendered_unsorted" >"$rendered"
  if cmp -s "$shared" "$rendered"; then
    return 0
  fi

  printf 'error: sync render destinations differ from the shared mirror manifest\n' >&2
  diff -u --label 'shared lint-exempt mirror paths' \
    --label 'sync render destinations' "$shared" "$rendered" >&2 || [ "$?" -eq 1 ]
  return 1
}

check_tracked_mirror_set() {
  local expected="$sync_tmp/tracked-mirrors.expected"
  local actual_unsorted="$sync_tmp/tracked-mirrors.actual.unsorted"
  local actual="$sync_tmp/tracked-mirrors.actual"

  if ! python3 "$mirror_policy" list >"$expected"; then
    printf 'error: cannot read shared Pelorus mirror manifest\n' >&2
    return 1
  fi

  if ! git -C "$repo_root" ls-files -- \
    ':(glob)core/include/libvmaf/pelorus/**' \
    ':(glob)core/src/interop/pelorus_*' \
    "$test_rel" >"$actual_unsorted"; then
    printf 'error: cannot enumerate tracked Pelorus mirror paths\n' >&2
    return 1
  fi
  LC_ALL=C sort "$actual_unsorted" >"$actual"

  if cmp -s "$expected" "$actual"; then
    return 0
  fi

  printf 'DRIFT: tracked exact-mirror path set differs from the manifest\n' >&2
  diff -u --label 'manifest-owned mirror paths' \
    --label 'tracked lint-exempt mirror paths' "$expected" "$actual" >&2 || [ "$?" -eq 1 ]
  return 1
}

if ! check_render_manifest; then
  exit 1
fi

if [ "$mode" = "check" ] && ! check_tracked_mirror_set; then
  drift=1
fi

for row in "${manifest[@]}"; do
  IFS='|' read -r rel_src rel_dst inc_from inc_to <<<"$row"
  dst="$repo_root/$rel_dst"

  if [ "$mode" = "update" ]; then
    render_vendor "$rel_src" "$inc_from" "$inc_to" >"$(staged_path "$rel_dst")"
    continue
  fi

  if [ ! -f "$dst" ]; then
    printf 'DRIFT: vendored file missing: %s\n' "$rel_dst" >&2
    drift=1
    continue
  fi

  # Compare real file bytes against the canonical banner/include transform.
  # Temporary files keep EOF state observable to cmp(1).
  expected="$sync_tmp/$(printf '%s' "$rel_dst" | tr '/' '_').expected"
  render_vendor "$rel_src" "$inc_from" "$inc_to" >"$expected"
  if ! cmp -s "$expected" "$dst"; then
    printf 'DRIFT: %s differs from pinned pelorus %s\n' "$rel_dst" "$rel_src" >&2
    diff -u --label "pelorus:$rel_src (transformed)" --label "$rel_dst" \
      "$expected" "$dst" >&2 || [ "$?" -eq 1 ]
    drift=1
  fi
done

# --- Conformance fixture --------------------------------------------------
# The fixture is rendered from two canonical inputs:
#   prefix = a deterministic VMAFx-authored license/provenance block whose pin
#            and ABI version come from the pinned Pelorus object.
#   body   = from the first vendored include onward; the verbatim Pelorus
#            test/interop_test.c body with "pelorus/" -> "libvmaf/pelorus/".
# The pelorus repo formats its C with the same .clang-format as vmafx (its
# config notes it "matches the vmafx sibling"), so the re-vendored body is
# already clang-format clean — we vendor it raw and do NOT reformat it. The
# complete rendered file is byte-sensitive through EOF, so prefix mutations,
# local reformats, and trailing-newline changes all fail closed.

# The fixture renderer's source, kept apart from the shell function so the
# function stays short.
_RENDER_FIXTURE_PY="$(
  cat <<'PY'
import re
import sys
from pathlib import Path

sha, interop_path, fixture_path, license_id = sys.argv[1:]
# Split so REUSE does not take the joined tag for the licence of this script.
tag = "SPDX-" + "License-Identifier:"
source = Path(fixture_path).read_bytes()
interop = Path(interop_path).read_bytes()

def abi_component(name):
    match = re.search(
        rb"(?m)^#define[ \t]+" + name.encode() + rb"[ \t]+([0-9]+)u?[ \t]*$",
        interop,
    )
    if match is None:
        raise SystemExit(f"Pelorus interop header has no {name}")
    return match.group(1).decode("ascii")

match = re.search(rb"(?m)^#include \"pelorus/", source)
if match is None:
    raise SystemExit("Pelorus fixture has no vendored include")
spdx = re.findall(rb"(?m)^.*" + tag.encode() + rb".*$", source[: match.start()])
if spdx != [f" * {tag} {license_id}".encode()]:
    raise SystemExit(f"Pelorus fixture does not declare exactly {license_id}: {spdx}")
body = source[match.start():].replace(
    b"#include \"pelorus/", b"#include \"libvmaf/pelorus/"
)
major = abi_component("PELORUS_ABI_MAJOR")
minor = abi_component("PELORUS_ABI_MINOR")
prefix = f"""/**
 *
 *  Copyright 2026 Lusoris
 *
 * {tag} {license_id}
 */

/*
 * test_pelorus_interop.c — vmafx side of the SHARED Pelorus interop ABI
 * conformance fixture (VMAFx/pelorus@{sha}
 * test/interop_test.c, ABI {major}.{minor}).
 *
 * Both repos run byte-for-byte the same checks against their own copy of
 * interop.c. A green run here proves vmafx's vendored parser (ADR-1113) is
 * byte-identical to pelorus's writer: a blob packed by pelorus parses in vmafx
 * and vice versa, and the forward/back-compat rules (R3, R4, R6) hold. Keep
 * this file in sync with the pelorus original via
 * scripts/sync-pelorus-interop.sh; the only intended local edit is the include
 * path rewrite ("pelorus/<x>.h" -> "libvmaf/pelorus/<x>.h").
 *
 * No external test framework — exit non-zero on first failure (does NOT use the
 * libvmaf minunit harness in test.c; it carries its own main()).
 */

""".encode()
sys.stdout.buffer.write(prefix + body)
PY
)"

# Emit the canonical VMAFx fixture (prefix plus rewritten Pelorus body).
render_fixture() {
  local pinned_interop="$sync_tmp/pinned-interop.h"
  local pinned_fixture="$sync_tmp/pinned-interop-test.c"
  read_src "include/pelorus/interop.h" >"$pinned_interop"
  read_src "test/interop_test.c" >"$pinned_fixture"
  python3 -c "$_RENDER_FIXTURE_PY" "$PELORUS_VENDOR_SHA" "$pinned_interop" "$pinned_fixture" \
    "$PELORUS_MIRROR_LICENSE"
}

if [ "$mode" = "update" ]; then
  render_fixture >"$(staged_path "$test_rel")"
  for row in "${manifest[@]}"; do
    IFS='|' read -r rel_src rel_dst inc_from inc_to <<<"$row"
    mv "$(staged_path "$rel_dst")" "$repo_root/$rel_dst"
    printf 're-vendored: %s\n' "$rel_dst"
  done
  mv "$(staged_path "$test_rel")" "$test_dst"
  printf 're-vendored: %s (full file)\n' "$test_rel"
fi

if [ "$mode" = "check" ]; then
  if [ ! -f "$test_dst" ]; then
    printf 'DRIFT: conformance fixture missing: %s\n' "$test_rel" >&2
    drift=1
  else
    expected_fixture="$sync_tmp/fixture.expected"
    render_fixture >"$expected_fixture"
    if ! cmp -s "$expected_fixture" "$test_dst"; then
      printf 'DRIFT: conformance fixture differs from canonical rendered source\n' >&2
      diff -u --label 'pelorus:test/interop_test.c (canonical transform)' \
        --label "$test_rel" "$expected_fixture" "$test_dst" >&2 || [ "$?" -eq 1 ]
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
  printf '  python3 scripts/ci/run_meson_test.py -- -C core/build-cpu test_pelorus_interop\n'
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
