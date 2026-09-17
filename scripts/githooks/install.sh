#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Install worktree-independent hooks; see ADR-1241.
set -euo pipefail
exec python3 "$(dirname "$0")/install.py"
