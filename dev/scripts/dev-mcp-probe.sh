#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# dev/scripts/dev-mcp-probe.sh — run a single smoke probe against the
# running dev-MCP container and print results to stdout.
#
# Usage:
#   ./dev/scripts/dev-mcp-probe.sh
#   ./dev/scripts/dev-mcp-probe.sh --container vmaf-dev-mcp

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CONTAINER="vmaf-dev-mcp"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --container)
      CONTAINER="$2"
      shift 2
      ;;
    *)
      echo "Unknown flag: $1" >&2
      exit 1
      ;;
  esac
done

TS="$(date +%Y%m%dT%H%M%S)"
OUTPUT_DIR="${REPO_ROOT}/.workingdir/dev-mcp-probes"
OUTPUT_FILE="${OUTPUT_DIR}/probe-${TS}.json"
mkdir -p "${OUTPUT_DIR}"

echo "[dev-mcp-probe] Running smoke probe at ${TS}…"

docker exec "${CONTAINER}" /workspace/dev/scripts/smoke-probe-loop.sh \
  --once \
  --output "/probes/probe-${TS}.json"

echo "[dev-mcp-probe] Probe written to ${OUTPUT_FILE}"
if command -v jq &>/dev/null; then
  jq . "${OUTPUT_FILE}"
else
  cat "${OUTPUT_FILE}"
fi
