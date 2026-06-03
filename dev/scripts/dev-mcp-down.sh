#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# dev/scripts/dev-mcp-down.sh — stop and remove the dev-MCP stack
#
# Usage:
#   ./dev/scripts/dev-mcp-down.sh           # stop only
#   ./dev/scripts/dev-mcp-down.sh --volumes # also remove volumes

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

EXTRA_FLAGS=""
if [ "${1:-}" = "--volumes" ]; then
  EXTRA_FLAGS="--volumes"
  echo "[dev-mcp-down] Removing stack + volumes…"
else
  echo "[dev-mcp-down] Stopping dev-MCP stack (volumes preserved)…"
fi

docker compose \
  --project-directory "${REPO_ROOT}" \
  -f "${REPO_ROOT}/dev/docker-compose.yml" \
  down ${EXTRA_FLAGS}

echo "[dev-mcp-down] Done."
