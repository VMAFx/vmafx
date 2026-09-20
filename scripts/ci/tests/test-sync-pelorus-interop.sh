#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# The Pelorus mirror guard must read its exact Git object. A plain directory or
# a checkout that lacks the pin must fail before working-tree bytes are read.

set -euo pipefail

while IFS= read -r fixture_git_variable; do
  unset "$fixture_git_variable"
done < <(compgen -A variable GIT_)
unset fixture_git_variable
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

mirror="${work}/vmafx"
plain="${work}/pelorus-plain"
missing="${work}/pelorus-missing"
source_repo="${work}/pelorus-source"
mkdir -p "${mirror}/scripts" "${mirror}/core/include/libvmaf" \
  "${mirror}/core/src" "${mirror}/core/test" "${plain}/libpelorus/include/pelorus" \
  "${plain}/libpelorus/src" "${plain}/libpelorus/test"
cp "${repo_root}/scripts/sync-pelorus-interop.sh" "${mirror}/scripts/"
cp -R "${repo_root}/core/include/libvmaf/pelorus" "${mirror}/core/include/libvmaf/"
cp -R "${repo_root}/core/src/interop" "${mirror}/core/src/"
cp "${repo_root}/core/test/test_pelorus_interop.c" "${mirror}/core/test/"
git init -q "${mirror}"
git -C "${mirror}" add scripts core

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
        printf "%s", buf; inblk = 0; buf = ""; next
      }
      next
    }
    { print }
  ' "$1"
}

while IFS='|' read -r rel_src rel_dst inc_from inc_to; do
  if [[ -n "${inc_from}" ]]; then
    strip_banner "${mirror}/${rel_dst}" |
      sed 's|#include "'"${inc_to}"'"|#include "'"${inc_from}"'"|' \
        >"${plain}/libpelorus/${rel_src}"
  else
    strip_banner "${mirror}/${rel_dst}" >"${plain}/libpelorus/${rel_src}"
  fi
done <<'MANIFEST'
include/pelorus/pelorus.h|core/include/libvmaf/pelorus/pelorus.h||
include/pelorus/interop.h|core/include/libvmaf/pelorus/interop.h|pelorus/pelorus.h|libvmaf/pelorus/pelorus.h
include/pelorus/deband.h|core/include/libvmaf/pelorus/deband.h|pelorus/pelorus.h|libvmaf/pelorus/pelorus.h
include/pelorus/denoise.h|core/include/libvmaf/pelorus/denoise.h|pelorus/pelorus.h|libvmaf/pelorus/pelorus.h
src/interop.c|core/src/interop/pelorus_interop.c|pelorus/interop.h|libvmaf/pelorus/interop.h
src/deband_params.c|core/src/interop/pelorus_deband_params.c|pelorus/deband.h|libvmaf/pelorus/deband.h
src/denoise_params.c|core/src/interop/pelorus_denoise_params.c|pelorus/denoise.h|libvmaf/pelorus/denoise.h
src/qp_report_csv.c|core/src/interop/pelorus_qp_report_csv.c|pelorus/interop.h|libvmaf/pelorus/interop.h
src/version.c|core/src/interop/pelorus_version.c|pelorus/pelorus.h|libvmaf/pelorus/pelorus.h
MANIFEST

{
  printf '/* reconstructed source fixture */\n'
  awk '/^#include "libvmaf\/pelorus\// { f = 1 } f { print }' \
    "${mirror}/core/test/test_pelorus_interop.c" |
    sed 's|#include "libvmaf/pelorus/|#include "pelorus/|'
} >"${plain}/libpelorus/test/interop_test.c"

cp -R "${plain}" "${missing}"
git -C "${missing}" init -q
git -C "${missing}" add libpelorus
git -C "${missing}" -c user.name=fixture -c user.email=fixture.invalid commit -q -m missing-fixture

cp -R "${plain}" "${source_repo}"
git -C "${source_repo}" init -q
git -C "${source_repo}" add libpelorus
git -C "${source_repo}" -c user.name=fixture -c user.email=fixture.invalid commit -q -m fixture
fixture_pin="$(git -C "${source_repo}" rev-parse HEAD)"
fail=0

# Rewrite bytes portably: BSD sed requires an argument after -i, while GNU sed
# does not. The fixture exercises the same path on Linux and macOS runners.
python3 - "${mirror}/scripts/sync-pelorus-interop.sh" "${fixture_pin}" <<'PY'
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
pin = sys.argv[2].encode("ascii")
data, replacements = re.subn(
    rb'^PELORUS_VENDOR_SHA="[0-9a-f]{40}"$',
    b'PELORUS_VENDOR_SHA="' + pin + b'"',
    path.read_bytes(),
    flags=re.MULTILINE,
)
assert replacements == 1
path.write_bytes(data)
PY

update_output=""
update_rc=0
update_output="$(
  cd "${mirror}" && scripts/sync-pelorus-interop.sh --update "${source_repo}" 2>&1
)" || update_rc=$?
if [[ "${update_rc}" -ne 0 ]]; then
  printf 'FAIL: synthetic re-pin update failed\n%s\n' "${update_output}" >&2
  fail=$((fail + 1))
elif ! grep -Fq "VMAFx/pelorus@${fixture_pin}" \
  "${mirror}/core/test/test_pelorus_interop.c"; then
  printf 'FAIL: synthetic re-pin left the fixture source banner stale\n%s\n' \
    "${update_output}" >&2
  fail=$((fail + 1))
else
  printf 'PASS: synthetic re-pin updates every rendered banner\n'
fi

expect_fail_closed() {
  local name="$1" source="$2" expected="$3" output rc=0
  output="$(cd "${mirror}" && scripts/sync-pelorus-interop.sh "${source}" 2>&1)" || rc=$?
  if [[ "${rc}" -eq 0 ]]; then
    printf 'FAIL: %s accepted working-tree fallback\n%s\n' "${name}" "${output}" >&2
    fail=$((fail + 1))
  elif ! grep -Fq "${expected}" <<<"${output}"; then
    printf 'FAIL: %s did not report %q\n%s\n' "${name}" "${expected}" "${output}" >&2
    fail=$((fail + 1))
  else
    printf 'PASS: %s fails closed\n' "${name}"
  fi
}

expect_fail_closed "plain directory" "${plain}" "must be a Git checkout"
expect_fail_closed "checkout missing pin" "${missing}" "pinned Pelorus commit is unavailable"

expect_clean() {
  local name="$1" output rc=0
  output="$(cd "${mirror}" && scripts/sync-pelorus-interop.sh "${source_repo}" 2>&1)" || rc=$?
  if [[ "${rc}" -ne 0 ]]; then
    printf 'FAIL: %s rejected an exact fixture\n%s\n' "${name}" "${output}" >&2
    fail=$((fail + 1))
  else
    printf 'PASS: %s accepts exact bytes\n' "${name}"
  fi
}

expect_drift() {
  local name="$1" expected="$2" output rc=0
  output="$(cd "${mirror}" && scripts/sync-pelorus-interop.sh "${source_repo}" 2>&1)" || rc=$?
  if [[ "${rc}" -eq 0 ]]; then
    printf 'FAIL: %s ignored mirror drift\n%s\n' "${name}" "${output}" >&2
    fail=$((fail + 1))
  elif ! grep -Fq "${expected}" <<<"${output}"; then
    printf 'FAIL: %s did not report %q\n%s\n' "${name}" "${expected}" "${output}" >&2
    fail=$((fail + 1))
  else
    printf 'PASS: %s detects mirror drift\n' "${name}"
  fi
}

expect_clean "byte-exact baseline"

manifest_eof="${mirror}/core/include/libvmaf/pelorus/pelorus.h"
python3 - "${manifest_eof}" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
data = path.read_bytes()
assert data.endswith(b"\n")
path.write_bytes(data[:-1])
PY
expect_drift "manifest missing final newline" "core/include/libvmaf/pelorus/pelorus.h differs"
printf '\n' >>"${manifest_eof}"

fixture_eof="${mirror}/core/test/test_pelorus_interop.c"
python3 - "${fixture_eof}" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
data = path.read_bytes()
assert data.endswith(b"\n")
path.write_bytes(data[:-1])
PY
expect_drift "fixture missing final newline" "conformance fixture differs"
printf '\n' >>"${fixture_eof}"
expect_clean "restored EOF bytes"

python3 - "${fixture_eof}" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
data = path.read_bytes()
old = b"SHARED Pelorus interop ABI"
assert data.count(old) == 1
path.write_bytes(data.replace(old, b"MUTATED Pelorus interop ABI"))
PY
expect_drift "fixture prefix mutation" "conformance fixture differs"
(
  cd "${mirror}"
  scripts/sync-pelorus-interop.sh --update "${source_repo}" >/dev/null
)
if grep -Fq "MUTATED Pelorus interop ABI" "${fixture_eof}"; then
  printf 'FAIL: --update preserved a mutated fixture prefix\n' >&2
  fail=$((fail + 1))
else
  printf 'PASS: --update restores the canonical fixture prefix\n'
fi
expect_clean "canonical fixture restored"

extra_mirror="core/include/libvmaf/pelorus/unmanifested.h"
printf '/* must not inherit exact-mirror lint exemptions */\n' >"${mirror}/${extra_mirror}"
git -C "${mirror}" add "${extra_mirror}"
expect_drift "unmanifested tracked mirror" "tracked exact-mirror path set differs"
git -C "${mirror}" rm -q -f -- "${extra_mirror}"
expect_clean "unmanifested mirror removed"

workflow="${repo_root}/.github/workflows/lint-and-format.yml"
for required in \
  'name: Resolve exact Pelorus vendor pin' \
  'repository: VMAFx/pelorus' \
  "ref: \${{ steps.pelorus-vendor.outputs.sha }}" \
  "run: scripts/sync-pelorus-interop.sh \"\${GITHUB_WORKSPACE}/.ci/pelorus\""; do
  if ! grep -Fq "${required}" "${workflow}"; then
    printf 'FAIL: required Pre-Commit workflow lost Pelorus contract: %s\n' "${required}" >&2
    fail=$((fail + 1))
  fi
done

[[ "${fail}" -eq 0 ]]
