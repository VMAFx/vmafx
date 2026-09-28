#!/usr/bin/env bash
# Test harness for scripts/ci/check-container-build.sh (ADR-1102, ADR-1346,
# ADR-1354).
#
# Proves the container-only publishing gate in both directions:
#
#   * it FAILS on a non-container (host) build, in every mode, and
#   * it PASSES on a container build, and on an artifact tree stamped by one.
#
# dev/Containerfile has two roots that write the marker. `build-deps` writes
# it and every later dev stage (`libvmaf-build`, the local `vmaf-dev-mcp`)
# inherits it. `release-build`, the Debian 13 release-track stage the native
# release compiles in (ADR-1354), writes the same bytes. The gate therefore
# accepts exactly one identity (`vmaf-dev-mcp`) and rejects the retired
# self-hosted runner title (`vmaf-sycl-arc-runner`, ADR-1178) plus near-miss
# spellings.
#
# The container is simulated by pointing VMAFX_CONTAINER_MARKER at a marker
# file inside $(mktemp -d) whose contents are byte-identical to the one
# dev/Containerfile writes to /etc/vmafx-dev-container. The host is simulated
# by pointing it at a path that does not exist. No test writes outside its
# temporary directory, and none needs Docker.
#
# Usage: bash scripts/ci/tests/test-check-container-build.sh
#
# Exit 0 on all-pass, 1 on any failure.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="${SCRIPT_DIR}/../check-container-build.sh"

if [ ! -f "$GATE" ]; then
  echo "test-check-container-build: gate script not found: $GATE" >&2
  exit 2
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

PASS=0
FAIL=0

# The exact marker dev/Containerfile bakes into the image.
CONTAINER_MARKER="${WORKDIR}/etc-vmafx-dev-container"
cat >"$CONTAINER_MARKER" <<'EOF'
vmafx_dev_container=1
image_title=vmaf-dev-mcp
containerfile=dev/Containerfile
source=https://github.com/VMAFx/vmafx
EOF

# The fixture above is only meaningful if it matches what dev/Containerfile
# actually bakes into the image. Extract the marker lines from the
# Containerfile and compare, so the fixture cannot silently drift away from
# the image and leave the gate untested against reality.
CONTAINERFILE="${SCRIPT_DIR}/../../../dev/Containerfile"
EXTRACTED="${WORKDIR}/extracted-marker"
if [ -f "$CONTAINERFILE" ]; then
  sed -n '1,/> \/etc\/vmafx-dev-container/p' "$CONTAINERFILE" |
    grep -E "^[[:space:]]+'[a-z_]+=.*'[[:space:]]*\\\\$" |
    sed -E "s/^[[:space:]]*'(.*)'[[:space:]]*\\\\$/\1/" >"$EXTRACTED" || true
  if diff -u "$CONTAINER_MARKER" "$EXTRACTED" >"${WORKDIR}/marker.diff" 2>&1; then
    PASS=$((PASS + 1))
    echo "ok   fixture matches the marker dev/Containerfile bakes"
  else
    FAIL=$((FAIL + 1))
    echo "FAIL fixture has drifted from dev/Containerfile's marker:"
    sed 's/^/       | /' "${WORKDIR}/marker.diff"
  fi
else
  echo "note dev/Containerfile not found at ${CONTAINERFILE}; drift check skipped"
fi

# next_from_after <line> — the line number of the first FROM after <line>, or
# one past the end of the file when that stage is the last one.
next_from_after() {
  awk -v after="$1" 'NR > after && /^FROM / { print NR; found = 1; exit }
    END { if (!found) print NR + 1 }' "$CONTAINERFILE"
}

# Exactly two stages write the marker, once each: build-deps (libvmaf-build
# inherits it through gpu-sdks) and release-build, the Debian 13 release-track
# root the native release compiles in (ADR-1354). release-build must root at
# RELEASE_BUILDER_BASE and write the same bytes as build-deps.
if [ -f "$CONTAINERFILE" ]; then
  writes="$(grep -c '> /etc/vmafx-dev-container' "$CONTAINERFILE" || true)"
  mapfile -t marker_lines < <(grep -n '> /etc/vmafx-dev-container' "$CONTAINERFILE" | cut -d: -f1)
  # `|| true`: a missing stage must reach the FAIL report below, not end the
  # suite silently through set -e and pipefail.
  deps_line="$(grep -n '^FROM .* AS build-deps$' "$CONTAINERFILE" | head -1 | cut -d: -f1 || true)"
  sdks_line="$(grep -n '^FROM build-deps AS gpu-sdks$' "$CONTAINERFILE" | head -1 | cut -d: -f1 ||
    true)"
  release_line="$(grep -n '^FROM [$]{RELEASE_BUILDER_BASE} AS release-build$' "$CONTAINERFILE" |
    head -1 | cut -d: -f1 || true)"
  # marker_in_stage <FROM line> — the marker write inside that stage, if any.
  marker_in_stage() {
    local from="$1" end line
    [ -n "$from" ] || return 0
    end="$(next_from_after "$from")"
    for line in "${marker_lines[@]}"; do
      if [ "$line" -gt "$from" ] && [ "$line" -lt "$end" ]; then
        echo "$line"
        return 0
      fi
    done
  }
  deps_marker="$(marker_in_stage "$deps_line")"
  release_marker="$(marker_in_stage "$release_line")"
  if [ "$writes" = "2" ] && [ -n "$deps_marker" ] && [ -n "$sdks_line" ] &&
    [ "$deps_marker" -lt "$sdks_line" ] &&
    grep -qx 'FROM gpu-sdks AS libvmaf-build' "$CONTAINERFILE"; then
    PASS=$((PASS + 1))
    echo "ok   build-deps writes the marker once and libvmaf-build inherits it"
  else
    FAIL=$((FAIL + 1))
    echo "FAIL marker is not written once in build-deps and inherited by libvmaf-build"
  fi

  RELEASE_EXTRACTED="${WORKDIR}/extracted-release-marker"
  if [ -n "$release_marker" ]; then
    sed -n "${release_line},${release_marker}p" "$CONTAINERFILE" |
      grep -E "^[[:space:]]+'[a-z_]+=.*'[[:space:]]*\\\\$" |
      sed -E "s/^[[:space:]]*'(.*)'[[:space:]]*\\\\$/\1/" >"$RELEASE_EXTRACTED" || true
  else
    : >"$RELEASE_EXTRACTED"
  fi
  if [ -n "$release_marker" ] && cmp -s "$CONTAINER_MARKER" "$RELEASE_EXTRACTED"; then
    PASS=$((PASS + 1))
    echo "ok   release-build roots at RELEASE_BUILDER_BASE and writes the same marker"
  else
    FAIL=$((FAIL + 1))
    echo "FAIL release-build must root at \${RELEASE_BUILDER_BASE} and write the build-deps marker"
    diff -u "$CONTAINER_MARKER" "$RELEASE_EXTRACTED" | sed 's/^/       | /' || true
  fi

  # The gate's accepted identity is the Containerfile's, not a second spelling.
  baked_title="$(sed -n 's/^image_title=//p' "$EXTRACTED")"
  if [ -n "$baked_title" ] && grep -qx "CANONICAL_TITLE=\"${baked_title}\"" "$GATE"; then
    PASS=$((PASS + 1))
    echo "ok   gate accepts exactly the title dev/Containerfile bakes (${baked_title})"
  else
    FAIL=$((FAIL + 1))
    echo "FAIL gate CANONICAL_TITLE does not match dev/Containerfile's '${baked_title}'"
  fi
fi

# A marker from some other project's container: present, but not ours.
FOREIGN_MARKER="${WORKDIR}/foreign-marker"
cat >"$FOREIGN_MARKER" <<'EOF'
image_title=some-other-image
EOF

# Our key, explicitly negated.
NEGATED_MARKER="${WORKDIR}/negated-marker"
cat >"$NEGATED_MARKER" <<'EOF'
vmafx_dev_container=0
image_title=vmaf-dev-mcp
EOF

# The retired ADR-1178 self-hosted runner identity. No image writes it (the
# runner image inherits the dev-container marker unchanged), and ADR-1346
# removed it from the accepted set, so it must now be rejected everywhere.
RUNNER_MARKER="${WORKDIR}/runner-marker"
cat >"$RUNNER_MARKER" <<'EOF'
vmafx_dev_container=1
image_title=vmaf-sycl-arc-runner
containerfile=dev/Containerfile.runner
source=https://github.com/VMAFx/vmafx
EOF

# write_marker_with_title <path> <title>
# A well-formed marker whose image_title is the given (possibly near-miss)
# value; used for the identity boundary cases below.
write_marker_with_title() {
  printf 'vmafx_dev_container=1\nimage_title=%s\ncontainerfile=dev/Containerfile\nsource=https://github.com/VMAFx/vmafx\n' \
    "$2" >"$1"
}

# Our key set, but image_title is unauthorized.
UNAUTHORIZED_TITLE_MARKER="${WORKDIR}/unauthorized-title-marker"
cat >"$UNAUTHORIZED_TITLE_MARKER" <<'EOF'
vmafx_dev_container=1
image_title=unauthorized-runner
containerfile=dev/Containerfile
source=https://github.com/VMAFx/vmafx
EOF

# Our key set, but the marker is truncated (no image_title).
TRUNCATED_MARKER="${WORKDIR}/truncated-marker"
printf 'vmafx_dev_container=1\n' >"$TRUNCATED_MARKER"

# A path that deliberately does not exist — this is "running on the host".
HOST_MARKER="${WORKDIR}/definitely-not-here/etc/vmafx-dev-container"

# run_case <label> <expected-exit> <marker-path> [gate args...]
run_case() {
  local label="$1" expected="$2" marker="$3"
  shift 3
  local rc=0
  VMAFX_CONTAINER_MARKER="$marker" bash "$GATE" "$@" \
    >"${WORKDIR}/out.log" 2>&1 || rc=$?
  if [ "$rc" -eq "$expected" ]; then
    PASS=$((PASS + 1))
    printf 'ok   %-58s (exit %d)\n' "$label" "$rc"
  else
    FAIL=$((FAIL + 1))
    printf 'FAIL %-58s (exit %d, expected %d)\n' "$label" "$rc" "$expected"
    sed 's/^/       | /' "${WORKDIR}/out.log"
  fi
}

echo "=== --assert: host must fail, container must pass ==="
run_case "assert on host (no marker)" 1 "$HOST_MARKER"
run_case "assert with a foreign container marker" 1 "$FOREIGN_MARKER"
run_case "assert with vmafx_dev_container=0" 1 "$NEGATED_MARKER"
run_case "assert with a truncated marker" 1 "$TRUNCATED_MARKER"
run_case "assert inside the dev container" 0 "$CONTAINER_MARKER"
run_case "assert (explicit --assert), container" 0 "$CONTAINER_MARKER" --assert
run_case "assert with the retired sycl-arc runner title" 1 "$RUNNER_MARKER"
run_case "assert with unauthorized image_title" 1 "$UNAUTHORIZED_TITLE_MARKER"
if [ -s "$EXTRACTED" ]; then
  run_case "assert with the marker build-deps writes" 0 "$EXTRACTED"
fi

echo
echo "=== identity boundaries: one exact title, no near misses ==="
CRLF_MARKER="${WORKDIR}/crlf-marker"
printf 'vmafx_dev_container=1\r\nimage_title=vmaf-dev-mcp\r\n' >"$CRLF_MARKER"
run_case "assert with a CRLF marker (CR is stripped)" 0 "$CRLF_MARKER"
NEAR_MARKER="${WORKDIR}/near-miss-marker"
for near_miss in vmafx-dev-mcp vmafx-sycl-arc-runner VMAF-DEV-MCP \
  vmaf-dev-mcp-build-deps "vmaf-dev-mcp " " vmaf-dev-mcp" vmaf-dev-mc; do
  write_marker_with_title "$NEAR_MARKER" "$near_miss"
  run_case "assert rejects image_title='${near_miss}'" 1 "$NEAR_MARKER"
done
FIRST_WINS_MARKER="${WORKDIR}/first-wins-marker"
printf 'vmafx_dev_container=1\nimage_title=vmaf-sycl-arc-runner\nimage_title=vmaf-dev-mcp\n' \
  >"$FIRST_WINS_MARKER"
run_case "assert reads the first image_title only" 1 "$FIRST_WINS_MARKER"

echo
echo "=== --stamp: only a container build may stamp an artifact tree ==="
HOST_ARTIFACTS="${WORKDIR}/host-artifacts"
CONTAINER_ARTIFACTS="${WORKDIR}/container-artifacts"
mkdir -p "$HOST_ARTIFACTS" "$CONTAINER_ARTIFACTS"
printf 'ELF-ish\n' >"${HOST_ARTIFACTS}/vmaf"
printf 'ELF-ish\n' >"${CONTAINER_ARTIFACTS}/vmaf"

run_case "stamp from a host build" 1 "$HOST_MARKER" --stamp "$HOST_ARTIFACTS"
if [ -e "${HOST_ARTIFACTS}/container-build-provenance.txt" ]; then
  FAIL=$((FAIL + 1))
  echo "FAIL a failed host stamp still wrote a provenance file"
else
  PASS=$((PASS + 1))
  echo "ok   a failed host stamp wrote no provenance file"
fi

run_case "stamp from inside the dev container" 0 "$CONTAINER_MARKER" --stamp "$CONTAINER_ARTIFACTS"
STAMP="${CONTAINER_ARTIFACTS}/container-build-provenance.txt"
if grep -qx 'schema=vmafx-container-build-provenance/1' "$STAMP" &&
  grep -qx 'vmafx_dev_container=1' "$STAMP" &&
  grep -qx 'image_title=vmaf-dev-mcp' "$STAMP"; then
  PASS=$((PASS + 1))
  echo "ok   container stamp carries schema, flag and image title"
else
  FAIL=$((FAIL + 1))
  echo "FAIL container stamp is malformed:"
  sed 's/^/       | /' "$STAMP"
fi

RUNNER_ARTIFACTS="${WORKDIR}/runner-artifacts"
mkdir -p "$RUNNER_ARTIFACTS"
printf 'ELF-ish\n' >"${RUNNER_ARTIFACTS}/vmaf"
run_case "stamp with the retired sycl-arc runner title fails" 1 "$RUNNER_MARKER" --stamp "$RUNNER_ARTIFACTS"
if [ -e "${RUNNER_ARTIFACTS}/container-build-provenance.txt" ]; then
  FAIL=$((FAIL + 1))
  echo "FAIL a rejected runner-title stamp still wrote a provenance file"
else
  PASS=$((PASS + 1))
  echo "ok   a rejected runner-title stamp wrote no provenance file"
fi

STAGE_ARTIFACTS="${WORKDIR}/build-deps-artifacts"
if [ -s "$EXTRACTED" ]; then
  mkdir -p "$STAGE_ARTIFACTS"
  printf 'ELF-ish\n' >"${STAGE_ARTIFACTS}/vmaf"
  GITHUB_SHA=0123456789abcdef0123456789abcdef01234567 SOURCE_DATE_EPOCH=0 \
    run_case "stamp inside the build-deps stage marker" 0 "$EXTRACTED" \
    --stamp "$STAGE_ARTIFACTS"
  STAGE_STAMP="${STAGE_ARTIFACTS}/container-build-provenance.txt"
  if grep -qx 'image_title=vmaf-dev-mcp' "$STAGE_STAMP" &&
    grep -qx 'containerfile=dev/Containerfile' "$STAGE_STAMP" &&
    grep -qx 'git_commit=0123456789abcdef0123456789abcdef01234567' "$STAGE_STAMP" &&
    grep -qx 'stamped_at=1970-01-01T00:00:00Z' "$STAGE_STAMP"; then
    PASS=$((PASS + 1))
    echo "ok   build-deps stamp records title, Containerfile, commit and epoch"
  else
    FAIL=$((FAIL + 1))
    echo "FAIL build-deps stamp is malformed:"
    sed 's/^/       | /' "$STAGE_STAMP"
  fi
fi

UNAUTH_ARTIFACTS="${WORKDIR}/unauth-artifacts"
mkdir -p "$UNAUTH_ARTIFACTS"
printf 'ELF-ish\n' >"${UNAUTH_ARTIFACTS}/vmaf"
run_case "stamp with unauthorized image_title fails" 1 "$UNAUTHORIZED_TITLE_MARKER" --stamp "$UNAUTH_ARTIFACTS"

echo
echo "=== --verify: fail-closed on anything but a valid stamp ==="
# Deliberately verified with the HOST marker: verification must not depend on
# the verifying job being containerised, only on the stamp.
run_case "verify a container-stamped tree (from host)" 0 "$HOST_MARKER" --verify "$CONTAINER_ARTIFACTS"
if [ -s "$EXTRACTED" ]; then
  run_case "verify a build-deps-stamped tree (from host)" 0 "$HOST_MARKER" --verify "$STAGE_ARTIFACTS"
fi
RETIRED_RUNNER_DIR="${WORKDIR}/retired-runner-stamp"
mkdir -p "$RETIRED_RUNNER_DIR"
cat >"${RETIRED_RUNNER_DIR}/container-build-provenance.txt" <<'EOF'
schema=vmafx-container-build-provenance/1
vmafx_dev_container=1
image_title=vmaf-sycl-arc-runner
EOF
run_case "verify a stamp with the retired runner title" 1 "$HOST_MARKER" --verify "$RETIRED_RUNNER_DIR"
run_case "verify an unstamped tree" 1 "$HOST_MARKER" --verify "$HOST_ARTIFACTS"

EMPTY_DIR="${WORKDIR}/empty-stamp"
mkdir -p "$EMPTY_DIR"
: >"${EMPTY_DIR}/container-build-provenance.txt"
run_case "verify an empty stamp file" 1 "$HOST_MARKER" --verify "$EMPTY_DIR"

BAD_SCHEMA_DIR="${WORKDIR}/bad-schema"
mkdir -p "$BAD_SCHEMA_DIR"
cat >"${BAD_SCHEMA_DIR}/container-build-provenance.txt" <<'EOF'
schema=some-other-thing/9
vmafx_dev_container=1
image_title=vmaf-dev-mcp
EOF
run_case "verify a stamp with an unknown schema" 1 "$HOST_MARKER" --verify "$BAD_SCHEMA_DIR"

FORGED_DIR="${WORKDIR}/negated-stamp"
mkdir -p "$FORGED_DIR"
cat >"${FORGED_DIR}/container-build-provenance.txt" <<'EOF'
schema=vmafx-container-build-provenance/1
vmafx_dev_container=0
image_title=vmaf-dev-mcp
EOF
run_case "verify a stamp that denies containerness" 1 "$HOST_MARKER" --verify "$FORGED_DIR"

UNAUTH_TITLE_DIR="${WORKDIR}/unauth-title-stamp"
mkdir -p "$UNAUTH_TITLE_DIR"
cat >"${UNAUTH_TITLE_DIR}/container-build-provenance.txt" <<'EOF'
schema=vmafx-container-build-provenance/1
vmafx_dev_container=1
image_title=unauthorized-runner
EOF
run_case "verify a stamp with an unauthorized image_title" 1 "$HOST_MARKER" --verify "$UNAUTH_TITLE_DIR"

TITLELESS_DIR="${WORKDIR}/titleless-stamp"
mkdir -p "$TITLELESS_DIR"
cat >"${TITLELESS_DIR}/container-build-provenance.txt" <<'EOF'
schema=vmafx-container-build-provenance/1
vmafx_dev_container=1
image_title=
EOF
run_case "verify a stamp with an empty image_title" 1 "$HOST_MARKER" --verify "$TITLELESS_DIR"

# A symlink to a valid stamp elsewhere is still not a staged stamp; the
# release verifier rejects one too, and the two gates must agree.
SYMLINK_DIR="${WORKDIR}/symlinked-stamp"
mkdir -p "$SYMLINK_DIR"
ln -s "${CONTAINER_ARTIFACTS}/container-build-provenance.txt" \
  "${SYMLINK_DIR}/container-build-provenance.txt"
run_case "verify a symlinked stamp (target is valid)" 1 "$HOST_MARKER" --verify "$SYMLINK_DIR"
if grep -q 'is a symlink, not a staged stamp' "${WORKDIR}/out.log"; then
  PASS=$((PASS + 1))
  echo "ok   symlinked stamp is named as a symlink"
else
  FAIL=$((FAIL + 1))
  echo "FAIL symlinked stamp rejection does not name the symlink:"
  sed 's/^/       | /' "${WORKDIR}/out.log"
fi
DANGLING_DIR="${WORKDIR}/dangling-stamp"
mkdir -p "$DANGLING_DIR"
ln -s "${WORKDIR}/no-such-stamp" "${DANGLING_DIR}/container-build-provenance.txt"
run_case "verify a dangling symlinked stamp" 1 "$HOST_MARKER" --verify "$DANGLING_DIR"

echo
echo "=== consumer verifier: verify-native-release-artifacts.sh fails closed ==="
VERIFY_NATIVE="${SCRIPT_DIR}/../../release/verify-native-release-artifacts.sh"
PROVENANCE_ERROR='staged container-build provenance is missing, empty, or a symlink'

# expect_native_provenance_failure <label> <dir>
# The verifier must fail on <dir> and name the provenance stamp as the reason.
expect_native_provenance_failure() {
  local label="$1" dir="$2" rc=0
  bash "$VERIFY_NATIVE" "$dir" 3.2.1 >"${WORKDIR}/verify-native.log" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ] && grep -qF "$PROVENANCE_ERROR" "${WORKDIR}/verify-native.log"; then
    PASS=$((PASS + 1))
    echo "ok   verify-native-release-artifacts.sh rejects ${label}"
  else
    FAIL=$((FAIL + 1))
    echo "FAIL verify-native-release-artifacts.sh on ${label} (exit ${rc}), expected: ${PROVENANCE_ERROR}"
    sed 's/^/       | /' "${WORKDIR}/verify-native.log"
  fi
}

if [ -f "$VERIFY_NATIVE" ]; then
  # Everything the verifier checks before the stamp is present and valid: an
  # executable regular `vmaf` and a non-empty regular `libvmaf.so`. Only the
  # provenance stamp is missing, so the stamp check is what must fail.
  NATIVE_DIR="${WORKDIR}/native-unstamped"
  mkdir -p "$NATIVE_DIR"
  printf 'ELF-ish\n' >"${NATIVE_DIR}/vmaf"
  chmod 0755 "${NATIVE_DIR}/vmaf"
  printf 'ELF-ish\n' >"${NATIVE_DIR}/libvmaf.so"
  expect_native_provenance_failure "an unstamped tree" "$NATIVE_DIR"

  NATIVE_LINKED_DIR="${WORKDIR}/native-symlinked-stamp"
  mkdir -p "$NATIVE_LINKED_DIR"
  cp -p "${NATIVE_DIR}/vmaf" "${NATIVE_DIR}/libvmaf.so" "$NATIVE_LINKED_DIR/"
  ln -s "${CONTAINER_ARTIFACTS}/container-build-provenance.txt" \
    "${NATIVE_LINKED_DIR}/container-build-provenance.txt"
  expect_native_provenance_failure "a symlinked stamp" "$NATIVE_LINKED_DIR"
fi

echo
echo "=== invocation errors exit 2 (never 0) ==="
run_case "--stamp with no directory" 2 "$CONTAINER_MARKER" --stamp
run_case "--verify with no directory" 2 "$CONTAINER_MARKER" --verify
run_case "--stamp on a missing directory" 2 "$CONTAINER_MARKER" --stamp "${WORKDIR}/nope"
run_case "--verify on a missing directory" 2 "$CONTAINER_MARKER" --verify "${WORKDIR}/nope"
run_case "unknown flag" 2 "$CONTAINER_MARKER" --containerise-please
run_case "--assert with a stray argument" 2 "$CONTAINER_MARKER" --assert extra

echo
echo "-----------------------------------------------------------"
echo "test-check-container-build: ${PASS} passed, ${FAIL} failed"
if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
exit 0
