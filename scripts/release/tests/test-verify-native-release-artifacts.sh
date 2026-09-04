#!/usr/bin/env bash
# Regression tests for the staged Linux release-artifact verifier.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VERIFY="$SCRIPT_DIR/../verify-native-release-artifacts.sh"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT INT TERM
pass=0
fail=0

check() {
  local description="$1"
  shift
  if "$@"; then
    printf 'PASS: %s\n' "$description"
    pass=$((pass + 1))
  else
    printf 'FAIL: %s\n' "$description" >&2
    fail=$((fail + 1))
  fi
}

expect_rejected() {
  local root="$1"
  local expected_version="${2:-3.2.1}"
  env -i PATH="$PATH" "$VERIFY" "$root" "$expected_version" \
    >/dev/null 2>&1 && return 1
  return 0
}

build="$scratch/build"
mkdir -p "$build"
printf '%s\n' \
  'int vmafx_fixture(void) {' \
  '    return 321;' \
  '}' >"$scratch/libvmaf.c"
printf '%s\n' \
  '#include <stdio.h>' \
  '#include <string.h>' \
  '' \
  'int vmafx_fixture(void);' \
  '' \
  'int main(int argc, char **argv) {' \
  '    if (argc == 2 && strcmp(argv[1], "--version") == 0 &&' \
  '        vmafx_fixture() == 321) {' \
  '        puts("3.2.1");' \
  '        return 0;' \
  '    }' \
  '    return 1;' \
  '}' >"$scratch/vmaf.c"
cc -fPIC -shared -Wl,-soname,libvmaf.so.3 \
  -o "$build/libvmaf.so.3.0.0" "$scratch/libvmaf.c"
ln -s libvmaf.so.3.0.0 "$build/libvmaf.so.3"
ln -s libvmaf.so.3 "$build/libvmaf.so"
cc -o "$build/vmaf" "$scratch/vmaf.c" -L"$build" -lvmaf

stage_fixture() {
  local destination="$1"
  mkdir -p "$destination"
  while IFS= read -r -d '' library; do
    cp -L -- "$library" "$destination/$(basename -- "$library")"
  done < <(
    find "$build" -maxdepth 1 \( -type f -o -type l \) \
      -name 'libvmaf.so*' -print0
  )
  cp -- "$build/vmaf" "$destination/vmaf"
  chmod +x "$destination/vmaf"
  cat >"$destination/container-build-provenance.txt" <<'EOF'
schema=vmafx-container-build-provenance/1
vmafx_dev_container=1
image_title=vmaf-dev-mcp
containerfile=dev/Containerfile
source=https://github.com/VMAFx/vmafx
git_commit=testcommit
stamped_at=1970-01-01T00:00:00Z
EOF
}

good="$scratch/good"
stage_fixture "$good"
check 'materialized SONAME chain runs in a clean environment' \
  env -i PATH="$PATH" "$VERIFY" "$good" 3.2.1

missing_soname="$scratch/missing-soname"
stage_fixture "$missing_soname"
rm -- "$missing_soname/libvmaf.so.3"
check 'missing SONAME filename is rejected' expect_rejected "$missing_soname"

missing_realname="$scratch/missing-realname"
stage_fixture "$missing_realname"
rm -- "$missing_realname/libvmaf.so.3.0.0"
check 'missing real-name filename is rejected' expect_rejected "$missing_realname"

leaked_symlink="$scratch/leaked-symlink"
stage_fixture "$leaked_symlink"
rm -- "$leaked_symlink/libvmaf.so"
ln -s libvmaf.so.3 "$leaked_symlink/libvmaf.so"
check 'artifact-upload-unsafe symlink is rejected' expect_rejected "$leaked_symlink"

different_bytes="$scratch/different-bytes"
stage_fixture "$different_bytes"
printf 'different\n' >>"$different_bytes/libvmaf.so.3"
check 'divergent materialized link-chain bytes are rejected' \
  expect_rejected "$different_bytes"

not_executable="$scratch/not-executable"
stage_fixture "$not_executable"
chmod -x "$not_executable/vmaf"
check 'non-executable CLI is rejected' expect_rejected "$not_executable"

wrong_version="$scratch/wrong-version"
stage_fixture "$wrong_version"
check 'CLI version mismatch is rejected' \
  expect_rejected "$wrong_version" 3.2.0

missing_provenance="$scratch/missing-provenance"
stage_fixture "$missing_provenance"
rm -- "$missing_provenance/container-build-provenance.txt"
check 'missing container-build provenance is rejected' \
  expect_rejected "$missing_provenance"

empty_provenance="$scratch/empty-provenance"
stage_fixture "$empty_provenance"
: >"$empty_provenance/container-build-provenance.txt"
check 'empty container-build provenance is rejected' \
  expect_rejected "$empty_provenance"

symlinked_provenance="$scratch/symlinked-provenance"
stage_fixture "$symlinked_provenance"
rm -- "$symlinked_provenance/container-build-provenance.txt"
ln -s "$scratch/vmaf.c" "$symlinked_provenance/container-build-provenance.txt"
check 'symlinked container-build provenance is rejected' \
  expect_rejected "$symlinked_provenance"

printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
