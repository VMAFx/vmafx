#!/usr/bin/env bash
# Install worktree-independent hooks; see ADR-1241.
set -euo pipefail
exec python3 "$(dirname "$0")/install.py"
