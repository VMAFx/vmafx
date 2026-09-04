#!/usr/bin/env bash
# Test harness for scripts/ci/check-container-build.sh (ADR-1102).
#
# Proves the container-only publishing gate in both directions:
#
#   * it FAILS on a non-container (host) build, in every mode, and
#   * it PASSES on a container build, and on an artifact tree stamped by one.
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
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

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

echo
echo "=== --verify: fail-closed on anything but a valid stamp ==="
# Deliberately verified with the HOST marker: verification must not depend on
# the verifying job being containerised, only on the stamp.
run_case "verify a container-stamped tree (from host)" 0 "$HOST_MARKER" --verify "$CONTAINER_ARTIFACTS"
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

TITLELESS_DIR="${WORKDIR}/titleless-stamp"
mkdir -p "$TITLELESS_DIR"
cat >"${TITLELESS_DIR}/container-build-provenance.txt" <<'EOF'
schema=vmafx-container-build-provenance/1
vmafx_dev_container=1
image_title=
EOF
run_case "verify a stamp with an empty image_title" 1 "$HOST_MARKER" --verify "$TITLELESS_DIR"

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
