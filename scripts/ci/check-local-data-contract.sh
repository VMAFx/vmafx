#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Enforce ADR-1277's separation of private state, corpus data, and tracked docs.
set -euo pipefail
export LC_ALL=C

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

state_root=.workingdir
corpus_root=.corpus
retired_root="${state_root}2"
failed=0

active_paths=(
  .clang-tidy .claude/skills .codex .cursor .gemini .github
  .gitignore .pre-commit-config.yaml .windsurfrules AGENTS.md CLAUDE.md
  ai cmd compat core dev model pkg python renovate.json scripts tools
  ':(glob)docs/*.md'
  docs/adr/_index_fragments/_header.md docs/ai docs/api docs/architecture
  docs/backends docs/development docs/mcp
  docs/metrics docs/principles.md docs/state.md docs/usage
  ':(exclude)docs/rebase-notes.md'
  ':(exclude)scripts/ci/tests/test-check-local-data-contract.sh'
)

report_paths() {
  local heading=$1 paths=$2
  printf 'error: %s:\n' "$heading" >&2
  printf '%s\n' "$paths" | sed 's/^/  /' >&2
  failed=1
}

# Praetor hard-codes the retired root as a private scratch root
# (cordanaLLM/praetor#641, ADR-1351). Its managed tail block in .gitignore,
# which `praetorctl audit` verifies byte for byte, ignores it, and its locked
# documentation gate under tools/markdownlint/ names it. Only those
# praetor-owned lines are tolerated; the local-existence check below still
# rejects a stale retired directory.
praetor_ignore_begin='# BEGIN praetor private artifacts (praetorctl adopt)'
praetor_ignore_end='# END praetor private artifacts'

praetor_block_has_line() {
  local line=$1
  [ -f .gitignore ] || return 1
  awk -v b="$praetor_ignore_begin" -v e="$praetor_ignore_end" -v n="$line" \
    '$0 == b { inside = 1; next } $0 == e { inside = 0 } NR == n && inside { found = 1 }
     END { exit found ? 0 : 1 }' .gitignore
}

# `git check-ignore -v` prints "<source>:<line>:<pattern>\t<path>" for the
# rule that decides the path.
praetor_owns_retired_ignore() {
  local verdict=$1 rule_file line pattern
  rule_file=${verdict%%:*}
  verdict=${verdict#*:}
  line=${verdict%%:*}
  pattern=${verdict#*:}
  pattern=${pattern%%$'\t'*}
  [ "$rule_file" = .gitignore ] && [ "$pattern" = "/$retired_root/" ] &&
    praetor_block_has_line "$line"
}

for local_root in "$state_root" "$corpus_root"; do
  tracked=$(git ls-files -- "$local_root" "$local_root/**")
  if [ -n "$tracked" ]; then
    report_paths "ignored local data is tracked under $local_root" "$tracked"
  fi
  if ! git check-ignore -q --no-index -- "$local_root/.contract-probe"; then
    printf 'error: local root %s must be ignored\n' "$local_root" >&2
    failed=1
  fi
done

tracked_retired=$(git ls-files -- "$retired_root" "$retired_root/**")
if [ -n "$tracked_retired" ]; then
  report_paths "retired workspace path is tracked" "$tracked_retired"
fi
if retired_ignore=$(git check-ignore -v --no-index -- "$retired_root/.contract-probe"); then
  if ! praetor_owns_retired_ignore "$retired_ignore"; then
    printf 'error: retired workspace path must remain visible to Git\n' >&2
    failed=1
  fi
fi
if [ -e "$retired_root" ] || [ -L "$retired_root" ]; then
  printf 'error: retired workspace path exists locally: %s\n' "$retired_root" >&2
  failed=1
fi

# The retired root must remain visible to Git so stale local state cannot hide
# (praetor's managed rule above is the one tolerated exception), and it must
# never inflate or leak into a Docker build context before this gate gets a
# chance to reject it.
for local_root in "$state_root" "$retired_root" "$corpus_root"; do
  if ! grep -Fqx -- "$local_root/" .dockerignore; then
    printf 'error: local root %s must be excluded from Docker contexts\n' "$local_root" >&2
    failed=1
  fi
done

retired_refs=$(git grep -n -I -F "$retired_root" -- "${active_paths[@]}" \
  ':(exclude)tools/markdownlint' 2>/dev/null || true)
praetor_rule_refs=$(printf '%s\n' "$retired_refs" |
  sed -n "s|^\.gitignore:\([0-9]*\):/$retired_root/\$|\1|p")
for line in $praetor_rule_refs; do
  if praetor_block_has_line "$line"; then
    retired_refs=$(printf '%s\n' "$retired_refs" | grep -vFx -- ".gitignore:$line:/$retired_root/" || true)
  fi
done
if [ -n "$retired_refs" ]; then
  report_paths "active files reference the retired workspace path" "$retired_refs"
fi

local_links=$(git grep -n -I -E \
  '\[[^]]+\]\([^)]*\.(workingdir2?|corpus)(/|\))' -- '*.md' 2>/dev/null || true)
if [ -n "$local_links" ]; then
  report_paths "Markdown links target ignored machine-local data" "$local_links"
fi

corpus_in_state=$(git grep -n -I -E \
  '\.workingdir/(netflix|chug|konvid-|lsvq|youtube-ugc|waterloo|live-vqc|bvi-dvc|aggregated|corpus_run|encodes)(/|[^[:alnum:]_-])' \
  -- "${active_paths[@]}" 2>/dev/null || true)
if [ -n "$corpus_in_state" ]; then
  report_paths "dataset or reusable derived-data path is placed under .workingdir" "$corpus_in_state"
fi

state_in_corpus=$(git grep -n -I -E \
  '\.corpus/(OPEN|BACKLOG|BUGS|QUESTIONS|STATE)\.md' \
  -- "${active_paths[@]}" 2>/dev/null || true)
if [ -n "$state_in_corpus" ]; then
  report_paths "session-state path is placed under .corpus" "$state_in_corpus"
fi

if [ "$failed" -ne 0 ]; then
  exit 1
fi

printf 'check-local-data-contract: OK (state, corpus, and tracked docs are separated)\n'
