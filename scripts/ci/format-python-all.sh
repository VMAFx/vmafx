#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
readonly repo_root
declare -a python_files=()

cd "$repo_root"
mapfile -d '' -t python_files < <(git ls-files -z -- '*.py' '*.pyi')
if ((${#python_files[@]} == 0)); then
    echo "python format: no tracked Python files"
    exit 0
fi

for tool in ruff black; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "python format: required tool not found: $tool" >&2
        exit 127
    fi
done

ruff check --fix-only "${python_files[@]}"
black "${python_files[@]}"
