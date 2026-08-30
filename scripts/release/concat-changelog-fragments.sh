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
# Splice contract (ADR-0900):
#   The Unreleased block lives between the "## [Unreleased] ..." header
#   and the *next bracketed* "## [version] - date" header that release-please
#   writes at release time. The boundary regex is `^## \[` — NOT `^## ` —
#   because fragment bodies may legitimately contain `## ` subheadings
#   (per-PR section headers like "## Vulkan submit-pool migration").
#   A bare `^## ` sentinel would treat those as boundaries and explode the
#   rendered body across re-runs of --write (the 23 k-line drift PR #332 /
#   PR #383 / PR #401 observed). The contract release-please honours: every
#   release header is `## [vX.Y.Z] - YYYY-MM-DD`; never `## Foo`.
#
# Fragment-body hygiene:
#   Stray `## ` headers inside a fragment body are demoted to `**bold**`
#   pseudo-headers at render time. Reasoning: they were never the right
#   shape (the renderer emits `### Section` itself) and they hurt the splice
#   contract above. Authors should write bullets, not in-fragment sections;
#   the demotion keeps history-rendered text legible without forcing a
#   cross-PR rewrite of 80+ existing fragments. New fragments are validated
#   by `--lint`.
#
# Copyright 2026 Lusoris
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

# Boundary regex (awk ERE): release-please writes "## [vX.Y.Z] - YYYY-MM-DD";
# the Unreleased block header is "## [Unreleased] ...". Anchoring on "## ["
# is the only sentinel safe against fragment-internal "## " headers. The
# literal "[" needs escaping inside the awk regex; we ship it pre-escaped so
# the `-v boundary=...` plumbing stays string-clean.
BOUNDARY_REGEX='^## \\['

# Emit one fragment with stray h1/h2 headers demoted to bold pseudo-headers.
# The renderer-emitted "### Section" heading is the only h3 the Unreleased
# block should carry; in-fragment headers would pollute both the visual
# tree and the splice contract.
emit_fragment() {
  local frag="$1"
  # Replace leading "# " or "## " with "**…**" using awk so we keep any
  # trailing newline and avoid sed -E portability gotchas.
  awk '
    /^# / {
      sub(/^# /, "")
      printf "**%s**\n", $0
      next
    }
    /^## / {
      sub(/^## /, "")
      printf "**%s**\n", $0
      next
    }
    { print }
  ' "$frag"
}

warn_unknown_subdirs() {
  # Surface (but do not fail on) fragments living in subdirs the renderer
  # does not know about. PR #384 / ADR-0892 cleaned up perf/ + performance/;
  # this guard keeps future drifts visible.
  local known_csv
  known_csv="$(printf '%s\n' "${SECTIONS[@]}" | LC_ALL=C sort | paste -sd, -)"
  local unknown
  unknown="$(find "$FRAG_ROOT" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' |
    LC_ALL=C sort |
    awk -v known="$known_csv" 'BEGIN { n=split(known, k, ","); for (i=1; i<=n; i++) ok[k[i]]=1 } !($0 in ok) { print }')"
  if [[ -n "$unknown" ]]; then
    while IFS= read -r d; do
      printf 'WARNING: changelog.d/%s/ is not a Keep-a-Changelog section; fragments here are SKIPPED.\n' "$d" >&2
    done <<<"$unknown"
  fi
}

render() {
  # Emit the Unreleased body (without the "## [Unreleased] ..." header).
  # The body always begins with the legacy archive (verbatim — no edits
  # to existing entries during migration) and then appends one section
  # per Keep-a-Changelog category from the fragment tree. Sections that
  # have no fragments are silently skipped; fragments under unknown
  # subdirs trigger a stderr warning (per PR #384 / ADR-0892).
  local section title dir frag
  warn_unknown_subdirs
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
        local first_in_section=1
        for frag in "${files[@]}"; do
          # Skip empty / whitespace-only fragments; warn so the author
          # notices the stub rather than silently dropping the entry.
          if [[ ! -s "$frag" ]] || ! grep -q '[^[:space:]]' "$frag"; then
            printf 'WARNING: %s is empty; skipping.\n' "${frag#"$REPO_ROOT"/}" >&2
            continue
          fi
          if [[ $first_in_section -eq 1 ]]; then
            printf '### %s\n\n' "$title"
            first_in_section=0
          fi
          emit_fragment "$frag"
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
# "## [Unreleased] ..." header and the next bracketed "## [vX.Y.Z]" heading
# (which marks the previous release's first section start). The boundary
# regex is `^## \[`, NOT `^## ` — see "Splice contract" in the file header
# for the 23 k-line drift this avoids. Implemented in awk for speed.

current_block="$(awk -v boundary="$BOUNDARY_REGEX" '
    /^## \[Unreleased\]/ {in_block=1; next}
    in_block && /^## \[(Unreleased|[0-9])/ {in_block=0}
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
# Clean up scratch files on any exit path (including interrupts) so we never
# leak temp files in $TMPDIR if the awk pipeline below trips set -e.
trap 'rm -f "$tmp_body" "$tmp_out"' EXIT
# Command substitution strips trailing newlines from "$rendered". The legacy
# archive ends with one blank line (separator before the next release header)
# so we re-emit the rendered body followed by an explicit blank line.
{
  printf '%s\n' "$rendered"
  printf '\n'
} >"$tmp_body"

awk -v boundary="$BOUNDARY_REGEX" '
    /^## \[Unreleased\]/ {print; in_block=1; next}
    in_block && /^## \[(Unreleased|[0-9])/ {in_block=0}
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

# Sanity check: the awk pipeline keys off the "## [Unreleased]" header. If the
# header is absent in $CHANGELOG (template drift, accidental deletion), the
# pipeline produces an unchanged copy and the in-place mv silently leaves the
# file untouched — caller has no signal. Refuse to overwrite in that case.
if ! grep -q '^## \[Unreleased\]' "$tmp_out"; then
  printf 'ERROR: rendered CHANGELOG.md missing "## [Unreleased]" header — aborting overwrite\n' >&2
  exit 1
fi

mv "$tmp_out" "$CHANGELOG"
printf 'CHANGELOG.md Unreleased block rewritten from changelog.d/.\n' >&2
