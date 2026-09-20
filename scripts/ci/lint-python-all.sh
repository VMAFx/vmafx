#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
readonly repo_root
declare -a python_files=()
declare -a direct_mypy_files=()
declare -a ai_script_files=()

cd "$repo_root"
mapfile -d '' -t python_files < <(git ls-files -z -- '*.py' '*.pyi')
if ((${#python_files[@]} == 0)); then
  echo "python lint: no tracked Python files"
  exit 0
fi

for tool in ruff black mypy; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "python lint: required tool not found: $tool" >&2
    exit 127
  fi
done

# Suppression comments are inventory, not exemptions (ADR-1267). Ruff must
# report the underlying diagnostic even when a line still carries `noqa`.
ruff check --ignore-noqa "${python_files[@]}"
black --check "${python_files[@]}"

# Mypy derives module names from package roots. Passing the hyphenated
# compat/python-vmaf path directly is invalid, while passing ai/src both as a
# source root and as ordinary files makes every vmaf_train module appear
# twice. Split only the invocation topology; every tracked file remains in one
# of the three mandatory checks below.
for file in "${python_files[@]}"; do
  case "$file" in
    ai/src/* | compat/python-vmaf/*) ;;
    ai/scripts/*) ai_script_files+=("${file#ai/scripts/}") ;;
    *) direct_mypy_files+=("$file") ;;
  esac
done

readonly project_mypy_path="$repo_root/ai/src:$repo_root/ai/scripts:$repo_root/compat:$repo_root/python"
MYPYPATH="$project_mypy_path" mypy "${direct_mypy_files[@]}"
MYPYPATH="$repo_root/ai/src" mypy -p aiutils -p corpus -p vmaf_train
# ai/scripts is a flat executable-module root. Check it from that root so
# sibling imports such as `_script_bootstrap` have one canonical module name.
(
  cd "$repo_root/ai/scripts"
  MYPYPATH="$repo_root/ai/src:$repo_root/ai/scripts" mypy "${ai_script_files[@]}"
)
# compat/vmaf is the tracked compatibility symlink whose package-safe name
# exposes every module below compat/python-vmaf without hiding that subtree.
MYPYPATH="$repo_root/compat" mypy -p vmaf
