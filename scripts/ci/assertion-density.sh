#!/usr/bin/env bash
# Power of 10 rule 5 — density check.
# Policy: every fork-added C function ≥MIN_LINES lines (default 20) must
# contain ≥1 assert() call. NASA/JPL recommends ≥2 per function on
# average; we enforce "any non-trivial fork-added function has at least
# one assert" in CI, and report the ≥2 average informationally.
#
# Scope: files whose copyright header contains a Lusoris copyright line
# in either the legacy "Lusoris and Claude (Anthropic)" format or the
# current "Copyright YYYY Lusoris" format (rebrand decided 2026-05-27;
# memory: project_copyright_lusoris_only). Upstream Netflix files are
# exempted (separate cleanup ticket).
#
# Exit 0 on pass, 1 on any fork-added function ≥MIN_LINES lines with zero asserts.

set -euo pipefail

MIN_LINES="${MIN_LINES:-20}"

# Collect fork-added files by header scan.
mapfile -t FILES < <(
  git ls-files 'core/src/**/*.c' 'core/src/**/*.cpp' 'core/tools/*.c' \
    2>/dev/null | while read -r f; do
    [ -f "$f" ] || continue
    if head -n 20 "$f" 2>/dev/null | grep -qE "(Lusoris and Claude|Copyright [0-9]+ Lusoris)"; then
      echo "$f"
    fi
  done
)

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "assertion-density: no fork-added files found; skipping"
  exit 0
fi

echo "assertion-density: scanning ${#FILES[@]} fork-added files"

# One awk pass per file; awk prints one line per function on stdout:
#   FILE:LINE NAME NLINES NASSERTS
# We then aggregate in bash.
tmpfile="$(mktemp)"
trap 'rm -f "$tmpfile"' EXIT

for f in "${FILES[@]}"; do
  awk -v FILE="$f" '
        # Function start heuristic:
        #   - line starts at column 0 (no leading whitespace)
        #   - first token is NOT a C/C++ keyword (if/for/while/switch/do/return/else/goto/case/default)
        #   - the line ends with `{` (definition-on-one-line)
        #   - or the line ends with `)` and the NEXT line is `{` (K&R opening brace on its own line)
        # Track brace depth until depth == 0 to find the end.

        function is_keyword(t) {
            return t == "if" || t == "for" || t == "while" || t == "switch" ||
                   t == "do" || t == "return" || t == "else" || t == "goto" ||
                   t == "case" || t == "default" || t == "using" || t == "namespace" ||
                   t == "struct" || t == "enum" || t == "union" || t == "typedef" ||
                   t == "extern" || t == "static" || t == "inline" || t == "const" ||
                   t == "volatile" || t == "auto" || t == "register"
        }

        function looks_like_funcdef(line) {
            # Must be column-0 (no leading ws) and contain an identifier(...) pattern
            if (line ~ /^[[:space:]]/) return 0
            if (line ~ /^#/) return 0           # preprocessor
            if (line ~ /^\/\//) return 0        # comment
            if (line ~ /^\/\*/) return 0
            # Must contain `(` and not start with a keyword as the first word
            if (line !~ /\(/) return 0
            # grab first word
            m = line
            sub(/[^a-zA-Z_].*/, "", m)
            if (m == "") return 0
            if (is_keyword(m)) return 0
            # Exclude typedef/struct declarations masquerading as funcs
            if (line ~ /^typedef/) return 0
            return 1
        }

        function extract_name(line) {
            # Find the identifier immediately before the first `(`.
            s = line
            sub(/\(.*/, "", s)       # drop everything from `(`
            sub(/[[:space:]]+$/, "", s)
            # take the last token
            n = split(s, parts, /[[:space:]*&]+/)
            return parts[n]
        }

        {
            line = $0

            if (!inside) {
                if (looks_like_funcdef(line)) {
                    # Does it end with `{` OR `)` (K&R style)?
                    if (line ~ /\{[[:space:]]*$/) {
                        start_line = NR
                        fname = extract_name(line)
                        inside = 1
                        depth = gsub(/\{/, "{", line) - gsub(/\}/, "}", line)
                        n_asserts = 0
                        next
                    } else if (line ~ /\)[[:space:]]*$/) {
                        # peek: candidate header line
                        pending_start = NR
                        pending_name = extract_name(line)
                        next
                    }
                } else if (pending_start && line ~ /^\{/) {
                    start_line = pending_start
                    fname = pending_name
                    inside = 1
                    depth = 1
                    n_asserts = 0
                    pending_start = 0
                    next
                } else {
                    pending_start = 0
                }
            } else {
                # Count both assert() (standard) and VMAF_ASSERT_DEBUG()
                # (fork-specific, zero-cost in release; see
                # core/include/libvmaf/vmaf_assert.h and
                # docs/principles.md §1.2 rule 5).
                if (line ~ /(^|[^a-zA-Z_])assert[[:space:]]*\(/) n_asserts++
                if (line ~ /VMAF_ASSERT_DEBUG[[:space:]]*\(/) n_asserts++
                ob = gsub(/\{/, "{", line)
                cb = gsub(/\}/, "}", line)
                depth += ob - cb
                if (depth <= 0) {
                    nl = NR - start_line
                    printf "%s:%d %s %d %d\n", FILE, start_line, fname, nl, n_asserts
                    inside = 0
                    depth = 0
                    n_asserts = 0
                }
            }
        }
    ' "$f" >>"$tmpfile"
done

total_funcs=0
total_asserts=0
fail=0

while read -r loc name nl na; do
  total_funcs=$((total_funcs + 1))
  total_asserts=$((total_asserts + na))
  if [ "$nl" -ge "$MIN_LINES" ] && [ "$na" -eq 0 ]; then
    echo "FAIL: $loc $name — ${nl} lines, 0 asserts" >&2
    fail=$((fail + 1))
  fi
done <"$tmpfile"

if [ "$total_funcs" -gt 0 ]; then
  avg=$(awk -v a="$total_asserts" -v f="$total_funcs" 'BEGIN{printf "%.2f", a/f}')
  echo
  echo "assertion-density: ${total_asserts} asserts across ${total_funcs} fork-added functions (avg ${avg})"
fi

if [ "$fail" -gt 0 ]; then
  echo "FAIL: ${fail} fork-added functions ≥${MIN_LINES} lines have zero asserts" >&2
  exit 1
fi

echo "PASS: every fork-added function ≥${MIN_LINES} lines has ≥1 assert"
