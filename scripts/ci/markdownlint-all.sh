#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -euo pipefail

readonly chunk_size=200
declare -a markdown_files=()
declare -a runner=()

mapfile -d '' -t markdown_files < <(git ls-files -z -- '*.md')
if ((${#markdown_files[@]} == 0)); then
  echo "markdownlint: no tracked Markdown files"
  exit 0
fi

if command -v markdownlint-cli2 >/dev/null 2>&1; then
  runner=(markdownlint-cli2)
elif command -v npx >/dev/null 2>&1; then
  runner=(npx --yes markdownlint-cli2)
else
  echo "markdownlint: markdownlint-cli2 or npx is required" >&2
  exit 127
fi

status=0
for ((offset = 0; offset < ${#markdown_files[@]}; offset += chunk_size)); do
  if ! "${runner[@]}" "${markdown_files[@]:offset:chunk_size}"; then
    status=1
  fi
done

exit "$status"
