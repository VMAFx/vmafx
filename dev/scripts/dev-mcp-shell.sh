#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# dev/scripts/dev-mcp-shell.sh — attach an interactive shell to dev-mcp
#
# Usage:
#   ./dev/scripts/dev-mcp-shell.sh          # bash inside dev-mcp container
#   ./dev/scripts/dev-mcp-shell.sh vmaf CMD # run CMD instead of bash

set -euo pipefail

CONTAINER="${1:-vmaf-dev-mcp}"
shift || true
CMD="${*:-bash}"

echo "[dev-mcp-shell] Attaching to ${CONTAINER}…"
# shellcheck disable=SC2086  # CMD may be multiple words intentionally
docker exec -it "${CONTAINER}" ${CMD}
