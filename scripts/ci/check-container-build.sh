#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# check-container-build.sh — enforcement gate for the container-only
# publishing policy (ADR-1102, docs/development/publishing.md).
#
# The policy says every canonical/published artifact is produced inside the
# `vmaf-dev-mcp` container or the self-hosted canonical runner environment
# and that a host-side build is diagnostic-only.
# Until this script existed the policy was documentation-only: nothing in the
# release path could tell a container build from a host build, so a host-built
# binary could be attached to a release with no signal at all.
#
# The gate has one source of truth: the marker file `/etc/vmafx-dev-container`,
# written by `dev/Containerfile` in its first (`build-deps`) stage and
# therefore inherited by every downstream stage (`gpu-sdks`, `libvmaf-build`,
# `go-build`, `dev-mcp`). The marker cannot exist on a host checkout unless
# somebody deliberately creates it.
#
# THREAT MODEL. This is an accident gate, not a security boundary. It catches
# "the release job silently ran meson on the runner host", which is the failure
# ADR-1102 exists to prevent. It does not defend against an operator who forges
# /etc/vmafx-dev-container; the cryptographic story for published bytes is
# cosign + SLSA provenance in `.github/workflows/supply-chain.yml`, not this.
#
# THREE MODES
#
#   check-container-build.sh [--assert]
#       Assert the *current process* runs inside the VMAFx dev container.
#       Fail-closed: anything other than a well-formed marker declaring
#       `vmafx_dev_container=1` exits 1.
#
#   check-container-build.sh --stamp <artifact-dir>
#       Assert containerness, then drop a provenance stamp
#       (`container-build-provenance.txt`) into the artifact directory. A host
#       build cannot produce the stamp, because the assertion runs first.
#
#   check-container-build.sh --verify <artifact-dir>
#       Verify a previously-stamped artifact directory. Runs anywhere (the
#       verifying job need not itself be containerised); fails closed when the
#       stamp is missing, truncated, of an unknown schema, or does not declare
#       a container build.
#
# The --stamp / --verify pair is the shape the release path needs: the build
# job stamps the staged `artifacts/` tree, and a later job (or a consumer)
# verifies it. See docs/development/publishing.md § Enforcement.
#
# ENVIRONMENT
#   VMAFX_CONTAINER_MARKER  Override the marker path. Exists so the unit test
#                           (scripts/ci/tests/test-check-container-build.sh)
#                           can simulate both a container and a host without
#                           writing to /etc. Not for production use.
#
# EXIT CODES
#   0  policy satisfied
#   1  policy violation (host build, or missing/invalid stamp)
#   2  bad invocation
#
# Usage examples:
#   bash scripts/ci/check-container-build.sh
#   bash scripts/ci/check-container-build.sh --stamp artifacts
#   bash scripts/ci/check-container-build.sh --verify artifacts

set -euo pipefail

MARKER_PATH="${VMAFX_CONTAINER_MARKER:-/etc/vmafx-dev-container}"
STAMP_NAME="container-build-provenance.txt"
STAMP_SCHEMA="vmafx-container-build-provenance/1"
POLICY_DOC="docs/development/publishing.md"
POLICY_ADR="docs/adr/1102-phase4b9-container-only-publishing.md"

usage() {
  cat >&2 <<'EOF'
usage: check-container-build.sh [--assert]
       check-container-build.sh --stamp  <artifact-dir>
       check-container-build.sh --verify <artifact-dir>
EOF
}

# read_field <key> <file>
# Echoes the value of the first `key=value` line, with any CR stripped.
# Echoes nothing (and succeeds) when the key is absent, so callers decide
# whether an empty value is fatal.
read_field() {
  local key="$1" file="$2" line
  line="$(grep -m1 -E "^${key}=" -- "$file" 2>/dev/null || true)"
  printf '%s' "${line#*=}" | tr -d '\r'
}

fail() {
  echo "::error::$*" >&2
  return 0
}

# ---------------------------------------------------------------------------
# assert_container — the fail-closed containerness predicate.
# ---------------------------------------------------------------------------
assert_container() {
  if [ ! -f "$MARKER_PATH" ]; then
    fail "container-only publishing policy violated: no VMAFx dev-container marker at ${MARKER_PATH}"
    {
      echo "This process is NOT running inside the vmaf-dev-mcp container or the canonical runner."
      echo "Canonical artifacts (release binaries, published images, CI"
      echo "artifacts consumed downstream) must be built in the container environment."
      echo "See ${POLICY_DOC} and ${POLICY_ADR}."
      echo
      echo "  docker compose --project-directory \"\$(git rev-parse --show-toplevel)\" \\"
      echo "    -f dev/docker-compose.yml build dev-mcp"
      echo "  docker exec vmaf-dev-mcp <build command>"
    } >&2
    return 1
  fi

  local declared
  declared="$(read_field vmafx_dev_container "$MARKER_PATH")"
  if [ "$declared" != "1" ]; then
    fail "marker ${MARKER_PATH} does not declare vmafx_dev_container=1 (got '${declared}')"
    echo "A foreign container is not the VMAFx dev container. See ${POLICY_DOC}." >&2
    return 1
  fi

  local title
  title="$(read_field image_title "$MARKER_PATH")"
  if [ -z "$title" ]; then
    fail "marker ${MARKER_PATH} is malformed: image_title is empty"
    return 1
  fi

  case "$title" in
    vmaf-dev-mcp | vmaf-sycl-arc-runner | vmafx-dev-mcp | vmafx-sycl-arc-runner)
      ;;
    *)
      fail "marker ${MARKER_PATH} carries unrecognized canonical image title: '${title}'"
      echo "Expected 'vmaf-dev-mcp' or 'vmaf-sycl-arc-runner'. See ${POLICY_DOC}." >&2
      return 1
      ;;
  esac

  echo "container-build: OK — inside '${title}' (marker ${MARKER_PATH})"
  return 0
}

# ---------------------------------------------------------------------------
# stamp_dir <dir> — assert containerness, then write the provenance stamp.
# ---------------------------------------------------------------------------
stamp_dir() {
  local dir="$1"
  if [ ! -d "$dir" ]; then
    echo "check-container-build: not a directory: ${dir}" >&2
    return 2
  fi

  assert_container || return 1

  local commit stamped_at
  commit="${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}"
  if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    stamped_at="$(date -u -d "@${SOURCE_DATE_EPOCH}" '+%Y-%m-%dT%H:%M:%SZ')"
  else
    stamped_at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  fi

  {
    echo "schema=${STAMP_SCHEMA}"
    echo "vmafx_dev_container=1"
    echo "image_title=$(read_field image_title "$MARKER_PATH")"
    echo "containerfile=$(read_field containerfile "$MARKER_PATH")"
    echo "source=$(read_field source "$MARKER_PATH")"
    echo "git_commit=${commit}"
    echo "stamped_at=${stamped_at}"
  } >"${dir}/${STAMP_NAME}"

  echo "container-build: stamped ${dir}/${STAMP_NAME}"
  return 0
}

# ---------------------------------------------------------------------------
# verify_dir <dir> — fail-closed check of a previously written stamp.
# ---------------------------------------------------------------------------
verify_dir() {
  local dir="$1" stamp="$1/${STAMP_NAME}"
  if [ ! -d "$dir" ]; then
    echo "check-container-build: not a directory: ${dir}" >&2
    return 2
  fi

  if [ ! -s "$stamp" ]; then
    fail "container-only publishing policy violated: ${stamp} is missing or empty"
    {
      echo "The artifact directory carries no container-build provenance, so"
      echo "it cannot be shown to have been produced inside the vmaf-dev-mcp"
      echo "container. Build it in the container and stamp it with:"
      echo "  scripts/ci/check-container-build.sh --stamp ${dir}"
      echo "See ${POLICY_DOC} and ${POLICY_ADR}."
    } >&2
    return 1
  fi

  local schema declared title
  schema="$(read_field schema "$stamp")"
  if [ "$schema" != "$STAMP_SCHEMA" ]; then
    fail "unknown provenance schema in ${stamp}: '${schema}' (expected '${STAMP_SCHEMA}')"
    return 1
  fi

  declared="$(read_field vmafx_dev_container "$stamp")"
  if [ "$declared" != "1" ]; then
    fail "${stamp} does not declare a container build (vmafx_dev_container='${declared}')"
    return 1
  fi

  title="$(read_field image_title "$stamp")"
  if [ -z "$title" ]; then
    fail "${stamp} is malformed: image_title is empty"
    return 1
  fi

  case "$title" in
    vmaf-dev-mcp | vmaf-sycl-arc-runner | vmafx-dev-mcp | vmafx-sycl-arc-runner)
      ;;
    *)
      fail "${stamp} carries unrecognized canonical image title: '${title}'"
      echo "Expected 'vmaf-dev-mcp' or 'vmaf-sycl-arc-runner'. See ${POLICY_DOC}." >&2
      return 1
      ;;
  esac

  echo "container-build: verified ${stamp} — built in '${title}'" \
    "at $(read_field stamped_at "$stamp") from commit $(read_field git_commit "$stamp")"
  return 0
}

# ---------------------------------------------------------------------------
# Argument handling
# ---------------------------------------------------------------------------
mode="assert"
target=""

case "${1:---assert}" in
  --assert)
    if [ "$#" -gt 1 ]; then
      usage
      exit 2
    fi
    ;;
  --stamp | --verify)
    if [ "$#" -ne 2 ] || [ -z "${2:-}" ]; then
      usage
      exit 2
    fi
    mode="${1#--}"
    target="$2"
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    echo "check-container-build: unknown argument: $1" >&2
    usage
    exit 2
    ;;
esac

rc=0
case "$mode" in
  assert) assert_container || rc=$? ;;
  stamp) stamp_dir "$target" || rc=$? ;;
  verify) verify_dir "$target" || rc=$? ;;
esac

exit "$rc"
