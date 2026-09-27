#!/usr/bin/env bash
# Regression tests for the staged Linux release-artifact verifier.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

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

# Run the verifier and require one specific exit status and, when given, a
# fixed string on stderr -- so a case that expects the version comparison to
# reject is not satisfied by an unrelated earlier failure.
expect_status() {
  local status="$1"
  local root="$2"
  local expected_version="$3"
  local stderr_needle="${4:-}"
  local actual=0
  env -i PATH="$PATH" "$VERIFY" "$root" "$expected_version" \
    >/dev/null 2>"$scratch/stderr" || actual=$?
  if [[ "$actual" -ne "$status" ]]; then
    printf '  exit %d, expected %d; stderr: %s\n' \
      "$actual" "$status" "$(cat -- "$scratch/stderr")" >&2
    return 1
  fi
  [[ -z "$stderr_needle" ]] || grep -qF -- "$stderr_needle" "$scratch/stderr"
}

build="$scratch/build"
mkdir -p "$build"
printf '%s\n' \
  'int vmafx_fixture(void) {' \
  '    return 321;' \
  '}' >"$scratch/libvmaf.c"
# The fixture CLI reports whatever VMAFX_FIXTURE_VERSION was compiled in, on
# stderr, exactly like core/tools/cli_parse.cpp prints vmaf_version().
printf '%s\n' \
  '#include <stdio.h>' \
  '#include <string.h>' \
  '' \
  'int vmafx_fixture(void);' \
  '' \
  'int main(int argc, char **argv) {' \
  '    if (argc == 2 && strcmp(argv[1], "--version") == 0 &&' \
  '        vmafx_fixture() == 321) {' \
  '        (void)fprintf(stderr, "%s\n", VMAFX_FIXTURE_VERSION);' \
  '        return 0;' \
  '    }' \
  '    return 1;' \
  '}' >"$scratch/vmaf.c"
cc -fPIC -shared -Wl,-soname,libvmaf.so.3 \
  -o "$build/libvmaf.so.3.0.0" "$scratch/libvmaf.c"
ln -s libvmaf.so.3.0.0 "$build/libvmaf.so.3"
ln -s libvmaf.so.3 "$build/libvmaf.so"

# build_cli REPORTED OUTPUT: compile a fixture CLI whose --version prints
# REPORTED. REPORTED is spliced into a C string literal, so `\n` in it becomes
# a real newline in the output.
build_cli() {
  cc -DVMAFX_FIXTURE_VERSION="\"$1\"" -o "$2" "$scratch/vmaf.c" \
    -L"$build" -lvmaf
}
build_cli '3.2.1' "$build/vmaf"

stage_fixture() {
  local destination="$1"
  local cli="${2:-$build/vmaf}"
  mkdir -p "$destination"
  while IFS= read -r -d '' library; do
    cp -L -- "$library" "$destination/$(basename -- "$library")"
  done < <(
    find "$build" -maxdepth 1 \( -type f -o -type l \) \
      -name 'libvmaf.so*' -print0
  )
  cp -- "$cli" "$destination/vmaf"
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

# stage_reporting REPORTED: stage a complete, otherwise-valid bundle whose CLI
# reports REPORTED into a fresh directory, and leave its path in
# $reporting_root (a global, not stdout, so the counter survives).
fixture_count=0
reporting_root=''
stage_reporting() {
  fixture_count=$((fixture_count + 1))
  reporting_root="$scratch/reports-$fixture_count"
  build_cli "$1" "$scratch/vmaf-$fixture_count"
  stage_fixture "$reporting_root" "$scratch/vmaf-$fixture_count"
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
  expect_status 1 "$wrong_version" 3.2.0 'staged vmaf reported'

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

# --- Release-version shapes (ADR-1201) -------------------------------------
#
# A release build checks out the tag, so core/include/meson.build's
# `git describe --tags --long --match 'v*.*.*'` reports
# `v<version>-0-g<object name>`; only a tagless checkout falls back to the bare
# meson project version. The describe strings below are the ones a real
# CPU-only build printed for tags v1.0.0-rc.1 and v1.0.0.
full_sha1='d0f0e7e241c2eb70ff286ac997b1ff64430bcbf5'

# REPORTED | EXPECTED | description
accepted_cases=(
  '1.0.0|1.0.0|final version, meson fallback form'
  '1.0.0-rc.1|1.0.0-rc.1|release candidate, meson fallback form'
  'v1.0.0-0-g8820048|1.0.0|final version, describe exactly on tag v1.0.0'
  'v1.0.0-rc.1-0-gd0f0e7e|1.0.0-rc.1|release candidate, describe exactly on tag v1.0.0-rc.1'
  'v1.0.0-rc.0-0-gabcdef0|1.0.0-rc.0|boundary: rc.0 is a valid candidate number'
  'v1.0.0-rc.10-0-gabcdef0|1.0.0-rc.10|boundary: multi-digit candidate number'
  'v10.20.30-0-gabcdef0|10.20.30|boundary: multi-digit version components'
  "v1.0.0-rc.1-0-g${full_sha1}|1.0.0-rc.1|boundary: unabbreviated 40-hex object name"
)
for case in "${accepted_cases[@]}"; do
  IFS='|' read -r reported expected description <<<"$case"
  stage_reporting "$reported"
  check "accepts $description ($reported)" \
    expect_status 0 "$reporting_root" "$expected"
done

# The expected-version argument itself must be the narrow ADR-1201 shape.
usage_rejected_versions=(
  '1.0.0-rc.01'
  '1.0.0-rc'
  '1.0.0-rc.'
  '1.0.0-rc.1.2'
  '1.0.0-RC.1'
  '1.0.0-beta.1'
  '1.0.0-alpha'
  'v1.0.0'
  'v1.0.0-rc.1'
  '01.0.0'
  '1.0'
  '1.0.0-rc.1-0-gd0f0e7e'
  ''
)
for expected in "${usage_rejected_versions[@]}"; do
  check "rejects expected version '$expected' as a usage error" \
    expect_status 64 "$good" "$expected" 'expected version must be'
done

# The CLI must report exactly the tagged version; each of these is the version
# check rejecting, not an earlier structural failure.
short_hex='d0f0e7'
long_hex="${full_sha1}${full_sha1:0:25}"
rejected_cases=(
  'v1.0.0-rc.2-0-gd0f0e7e|1.0.0-rc.1|another candidate'
  'v1.0.0-0-g8820048|1.0.0-rc.1|the final build offered as a candidate'
  'v1.0.0-rc.1-0-gd0f0e7e|1.0.0|a candidate build offered as the final'
  '1.0.0-rc.1|1.0.0|bare candidate fallback offered as the final'
  '1.0.0|1.0.0-rc.1|bare final fallback offered as a candidate'
  'v1.0.0-rc.10-0-gabcdef0|1.0.0-rc.1|prefix trap: rc.10 for rc.1'
  '1.0.0-rc.10|1.0.0-rc.1|prefix trap: bare rc.10 for rc.1'
  'v1.0.0-rc.1-1-g8820048|1.0.0-rc.1|one commit after the candidate tag'
  'v1.0.0-12-gabcdef0|1.0.0|twelve commits after the final tag'
  'v1.0.0-rc.1-0-gd0f0e7e-dirty|1.0.0-rc.1|dirty describe suffix'
  '1.0.0-rc.1-dirty|1.0.0-rc.1|dirty suffix on the fallback form'
  'v1.0.0-rc.01-0-gd0f0e7e|1.0.0-rc.1|leading-zero candidate tag rc.01'
  "v1.0.0-rc.1-0-g${short_hex}|1.0.0-rc.1|object name shorter than 7 hex digits"
  "v1.0.0-rc.1-0-g${long_hex}|1.0.0-rc.1|object name longer than 64 hex digits"
  'v1.0.0-rc.1-0-gD0F0E7E|1.0.0-rc.1|upper-case object name'
  'v1.0.0-rc.1-0-gzzzzzzz|1.0.0-rc.1|non-hex object name'
  'v1.0.0-rc.1-0-g|1.0.0-rc.1|empty object name'
  'v1.0.0-rc.1-0-d0f0e7e|1.0.0-rc.1|object name without the g marker'
  '1.0.0-rc.1-0-gd0f0e7e|1.0.0-rc.1|describe form without the leading v'
  'v1.0.0-rc.1|1.0.0-rc.1|short describe form without --long'
  'v1.0.0|1.0.0|bare tag name for the final'
  'd0f0e7e|1.0.0-rc.1|bare commit abbreviation (describe --always)'
  'VMAFX v1.0.0-rc.1-0-gd0f0e7e (auto-backend, precision=max)|1.0.0-rc.1|vmafx-mode banner'
  ' 1.0.0-rc.1|1.0.0-rc.1|leading whitespace'
  'v1.0.0-rc.1-0-gd0f0e7e\nwarning: extra|1.0.0-rc.1|trailing extra output line'
)
for case in "${rejected_cases[@]}"; do
  IFS='|' read -r reported expected description <<<"$case"
  stage_reporting "$reported"
  check "rejects $description ($reported)" \
    expect_status 1 "$reporting_root" "$expected" 'staged vmaf reported'
done

printf '\n=== Results: %d passed, %d failed ===\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
