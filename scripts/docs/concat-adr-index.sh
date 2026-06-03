#!/usr/bin/env bash
# Concatenate per-ADR index fragments under docs/adr/_index_fragments/*.md
# into the rendered ADR index table that lives in docs/adr/README.md.
#
# Inputs:
#   $REPO_ROOT/docs/adr/_index_fragments/_header.md   verbatim README prelude
#       (everything before the "## Index" table — the table row itself stays
#       in this file too).
#   $REPO_ROOT/docs/adr/_index_fragments/NNNN-slug.md one Markdown table
#       row per ADR, named by full ADR slug (the same NNNN-kebab-case
#       used for the ADR file itself). Slug-keyed for historical
#       reasons: the 2026-05-02 dedup sweep renumbered duplicate-NNNN
#       ADRs (`0199-tiny-ai-netflix-training-corpus.md` →
#       `0242-…`, etc.); slug filenames remain stable across that
#       remap, so fragments survive the renumber without churn.
#       Rows render oldest-first by ADR ID (matches the existing
#       README order — ADR-0001 at the top, latest at the bottom).
#
# Outputs (stdout): the rendered docs/adr/README.md body.
#
# Flags:
#   --check    Compare against the in-tree docs/adr/README.md; exit
#              non-zero on drift.
#   --write    Rewrite docs/adr/README.md from fragments.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
FRAG_ROOT="$REPO_ROOT/docs/adr/_index_fragments"
README="$REPO_ROOT/docs/adr/README.md"
HEADER="$FRAG_ROOT/_header.md"

if [[ ! -f "$HEADER" ]]; then
  printf 'missing %s — header fragment required\n' "$HEADER" >&2
  exit 65
fi

ORDER_FILE="$FRAG_ROOT/_order.txt"

render() {
  cat "$HEADER"
  # Order is driven by _order.txt (frozen at migration time to preserve
  # the existing README row order, which is commit-merge-order rather
  # than strict numeric). New PRs append their slug to _order.txt — this
  # is the only line both PRs collide on, and conflict resolution is
  # trivial (concatenate both lines). Any fragment file not yet listed in
  # _order.txt is emitted afterwards in lexical (numeric) order so
  # forgotten manifest updates do not silently drop rows.
  declare -A seen=()
  if [[ -f "$ORDER_FILE" ]]; then
    while IFS= read -r slug; do
      [[ -z "$slug" || "$slug" == \#* ]] && continue
      local frag="$FRAG_ROOT/$slug.md"
      if [[ -f "$frag" ]]; then
        cat "$frag"
        # Each fragment is a single table row terminated by exactly
        # one newline (validated via `tail -c1`). Do NOT append an
        # extra newline — Markdown tables require contiguous rows.
        seen["$slug"]=1
      else
        printf 'WARNING: _order.txt lists missing fragment %s\n' "$slug" >&2
      fi
    done <"$ORDER_FILE"
  fi
  # Tail: any fragment not listed in the manifest, lexically sorted.
  local frag slug
  while IFS= read -r frag; do
    slug="$(basename "$frag" .md)"
    [[ -n "${seen[$slug]:-}" ]] && continue
    cat "$frag"
  done < <(find "$FRAG_ROOT" -maxdepth 1 -type f -name '[0-9]*.md' \
    ! -name '_*' | LC_ALL=C sort)
}

mode="render"
case "${1:-}" in
  --check) mode="check" ;;
  --write) mode="write" ;;
  --help | -h)
    sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  "") ;;
  *)
    printf 'unknown flag: %s\n' "$1" >&2
    exit 64
    ;;
esac

rendered="$(render)"

if [[ "$mode" == render ]]; then
  # printf '%s\n' restores the trailing newline that command substitution
  # stripped from "$rendered" — the README ends in a newline by convention.
  printf '%s\n' "$rendered"
  exit 0
fi

if [[ "$mode" == check ]]; then
  if diff -u "$README" <(printf '%s\n' "$rendered") >/dev/null; then
    exit 0
  fi
  diff -u "$README" <(printf '%s\n' "$rendered") || true
  printf '\ndocs/adr/README.md is out of sync with docs/adr/_index_fragments/.\n' >&2
  printf 'Run: scripts/docs/concat-adr-index.sh --write\n' >&2
  exit 1
fi

# --write
printf '%s\n' "$rendered" >"$README"
printf 'docs/adr/README.md rewritten from docs/adr/_index_fragments/.\n' >&2
