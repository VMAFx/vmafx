#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
# VMAFx managed hook dispatcher (ADR-1241). Installed as a regular file.
set -euo pipefail

mode=framework
hook_type="${0##*/}"
repo_root="$(git rev-parse --show-toplevel)"
hook_dir="$(cd "$(dirname "$0")" && pwd)"
cd "$repo_root"

if [ "$hook_type" = pre-rebase ]; then
  exec "$repo_root/scripts/git-hooks/pre-rebase" "$@"
fi

if [ "$hook_type" = pre-commit ] && [ "$mode" = native ]; then
  # Preserve the explicit ADR-0924 formatter-only opt-in. Other hook types
  # always use the framework, including Conventional Commits validation.
  exec "$repo_root/scripts/githooks/pre-commit.sh" "$@"
fi

if ! command -v pre-commit >/dev/null 2>&1; then
  echo "VMAFx hooks: pre-commit is missing; activate the environment used by make install-hooks." >&2
  exit 1
fi

# hook-impl preserves Git's arguments, push-ref stdin, and existing .legacy
# hooks. Never replace this with run --all-files: it loses the pushed range.
exec pre-commit hook-impl --config=.pre-commit-config.yaml \
  --hook-type "$hook_type" --hook-dir "$hook_dir" -- "$@"
