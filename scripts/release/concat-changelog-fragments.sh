#!/usr/bin/env bash
# Concatenate per-PR changelog fragments under changelog.d/<section>/*.md
# into the rendered "Unreleased" body that lives in CHANGELOG.md.
#
# Sections are emitted in the Keep-a-Changelog order:
#   Added → Changed → Deprecated → Removed → Fixed → Security
# Each fragment is a stand-alone Markdown bullet (or block of bullets) and
# may end with an optional trailing newline; the script preserves layout.
#
# Inputs:
#   $REPO_ROOT/changelog.d/<section>/*.md   per-PR fragments
#   $REPO_ROOT/changelog.d/_pre_fragment_legacy.md  (optional) content
#       migrated from the pre-fragment Unreleased block; emitted verbatim
#       at the top of the Unreleased body so the existing entries are not
#       lost when the system flips on.
#
# Outputs (stdout): the rendered Unreleased body. Caller is responsible
# for splicing it into CHANGELOG.md (release-please does this at release
# time; CI runs --check to gate drift).
#
# Flags:
#   --check    Compare output against the in-tree CHANGELOG.md Unreleased
#              block; exit non-zero on drift. Used by the docs-fragments
#              CI lane.
#   --write    Rewrite CHANGELOG.md in place with the rendered body.
#
# Copyright 2026 Lusoris and Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
FRAG_ROOT="$REPO_ROOT/changelog.d"
CHANGELOG="$REPO_ROOT/CHANGELOG.md"
LEGACY="$FRAG_ROOT/_pre_fragment_legacy.md"

# Keep-a-Changelog section order. Add/Changed/Deprecated/Removed/Fixed/Security.
SECTIONS=(added changed deprecated removed fixed security)
SECTION_TITLES=(Added Changed Deprecated Removed Fixed Security)

render() {
  # Emit the Unreleased body (without the "## [Unreleased] ..." header).
  # The body always begins with the legacy archive (verbatim — no edits
  # to existing entries during migration) and then appends one section
  # per Keep-a-Changelog category from the fragment tree. Sections that
  # have no fragments are silently skipped.
  local section title dir frag
  if [[ -f "$LEGACY" ]]; then
    cat "$LEGACY"
  fi
  for i in "${!SECTIONS[@]}"; do
    section="${SECTIONS[$i]}"
    title="${SECTION_TITLES[$i]}"
    dir="$FRAG_ROOT/$section"
    if [[ -d "$dir" ]]; then
      # Skip dotfiles. Lexical sort; contributors prefix filenames
      # with task IDs (e.g. T7-12-foo.md) for implicit ordering.
      local files
      mapfile -t files < <(find "$dir" -maxdepth 1 -type f -name '*.md' \
        ! -name '.*' | LC_ALL=C sort)
      if [[ ${#files[@]} -gt 0 ]]; then
        printf '### %s\n\n' "$title"
        for frag in "${files[@]}"; do
          cat "$frag"
          # Each fragment ends in newline; ensure exactly one blank
          # line follows so neighbouring bullets don't fuse.
          [[ "$(tail -c1 "$frag")" == $'\n' ]] || printf '\n'
          printf '\n'
        done
      fi
    fi
  done
}

mode="render"
case "${1:-}" in
  --check) mode="check" ;;
  --write) mode="write" ;;
  --help | -h)
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
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
  # stripped from "$rendered" — CHANGELOG.md is newline-terminated.
  printf '%s\n' "$rendered"
  exit 0
fi

# --check / --write splice the rendered body into CHANGELOG.md between the
# "## [Unreleased] ..." header and the next "## " heading (which marks the
# previous release's first section start). Implemented in awk for speed.

current_block="$(awk '
    /^## \[Unreleased\]/ {in_block=1; next}
    in_block && /^## [^[]/ {in_block=0}
    in_block {print}
' "$CHANGELOG")"

if [[ "$mode" == check ]]; then
  if [[ "$current_block" == "$rendered"* || "$rendered" == "$current_block"* ]]; then
    # Allow rendered to be a prefix-superset of current (extra trailing newlines)
    # but on real drift, fail loud.
    diff <(printf '%s' "$current_block") <(printf '%s' "$rendered") >/dev/null && exit 0
  fi
  diff -u <(printf '%s' "$current_block") <(printf '%s' "$rendered") || true
  printf '\nCHANGELOG.md Unreleased block is out of sync with changelog.d/.\n' >&2
  printf 'Run: scripts/release/concat-changelog-fragments.sh --write\n' >&2
  exit 1
fi

# --write — splice rendered body in place of the existing Unreleased block.
# Implemented via two passes (head before the block + body file + tail after)
# so the rendered text never has to fit in an argv variable.
tmp_body="$(mktemp)"
tmp_out="$(mktemp)"
trap 'rm -f "$tmp_body" "$tmp_out"' EXIT
# Command substitution strips trailing newlines from "$rendered". The legacy
# archive ends with one blank line (separator before the next release header)
# so we re-emit the rendered body followed by an explicit blank line.
{
  printf '%s\n' "$rendered"
  printf '\n'
} >"$tmp_body"

awk '
    /^## \[Unreleased\]/ {print; in_block=1; next}
    in_block && /^## [^[]/ {in_block=0}
    !in_block {print}
' "$CHANGELOG" | awk -v body="$tmp_body" '
    /^## \[Unreleased\]/ {
        print
        # Body file already begins with the blank line that separates the
        # Unreleased header from the first "### Section" — do NOT inject
        # another one here.
        while ((getline line < body) > 0) print line
        close(body)
        next
    }
    {print}
' >"$tmp_out"

mv "$tmp_out" "$CHANGELOG"
printf 'CHANGELOG.md Unreleased block rewritten from changelog.d/.\n' >&2
