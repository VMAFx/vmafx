#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Power of 10 rule 5 — density check.
# Policy: every project C-family function ≥MIN_LINES lines (default 20) must
# contain ≥1 assert() call. NASA/JPL recommends ≥2 per function on
# average; we enforce "any non-trivial project function has at least
# one assert" in CI, and report the ≥2 average informationally.
#
# Scope: every tracked C-family source, regardless of whether it originated in
# Netflix, another vendor, or the fork. ADR-1267 forbids origin-based tiers.
# Generated tracked sources are checked here too; fix their generator rather
# than excluding the output.
#
# Exit 0 on pass, 1 on any project function ≥MIN_LINES lines with zero asserts.

set -euo pipefail

MIN_LINES="${MIN_LINES:-20}"

# Git supplies only tracked paths, so build output is absent without a brittle
# directory denylist. NUL delimiters preserve unusual but valid filenames.
mapfile -d '' -t FILES < <(
    git ls-files -z -- '*.c' '*.cc' '*.cpp' '*.cxx' '*.cu' '*.hip' '*.m' '*.mm' 2>/dev/null
)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "assertion-density: no tracked C-family sources found; skipping"
    exit 0
fi

echo "assertion-density: scanning ${#FILES[@]} tracked project files"

# One awk pass per file; awk prints one line per function on stdout:
#   FILE:LINE NAME NLINES NASSERTS
# We then aggregate in bash.
tmpfile="$(mktemp)"
trap 'rm -f "$tmpfile"' EXIT

for f in "${FILES[@]}"; do
    awk -v FILE="$f" '
        # Function start heuristic:
        #   - line starts at column 0 (no leading whitespace)
        #   - the identifier immediately before `(` is not a control keyword
        #   - the line ends with `{` (definition-on-one-line)
        #   - or the line ends with `)` and the NEXT line is `{` (K&R opening brace on its own line)
        # Track brace depth until depth == 0 to find the end.

        function is_keyword(t) {
            return t == "if" || t == "for" || t == "while" || t == "switch" ||
                   t == "do" || t == "return" || t == "else" || t == "goto" ||
                   t == "case" || t == "default" || t == "catch" ||
                   t == "sizeof" || t == "alignof" || t == "decltype" ||
                   t == "static_assert"
        }

        function looks_like_funcdef(line) {
            # Must be column-0 (no leading ws) and contain an identifier(...) pattern
            if (line ~ /^[[:space:]]/) return 0
            if (line ~ /^#/) return 0           # preprocessor
            if (line ~ /^\/\//) return 0        # comment
            if (line ~ /^\/\*/) return 0
            # Must contain `(` and name a function rather than a control-flow
            # construct. Storage-class and return-type tokens such as `static`,
            # `extern`, `inline`, `const`, `struct`, and `enum` are deliberately
            # allowed: excluding them silently hid most ordinary definitions.
            if (line !~ /\(/) return 0
            m = extract_name(line)
            if (m !~ /^(~?[a-zA-Z_][a-zA-Z0-9_]*)(::(~?[a-zA-Z_][a-zA-Z0-9_]*))*$/) return 0
            sub(/^.*::/, "", m)
            if (is_keyword(m)) return 0
            # Exclude typedef/struct declarations masquerading as funcs
            if (line ~ /^typedef/) return 0
            return 1
        }

        function extract_name(line,    i, c, depth, prefix, token, base, name) {
            # Find the last identifier that opens a top-level parenthesis list.
            # This skips return-type wrappers such as CJSON_PUBLIC(type) while
            # ignoring calls/default expressions nested inside the real
            # parameter list.
            depth = 0
            name = ""
            for (i = 1; i <= length(line); i++) {
                c = substr(line, i, 1)
                if (c == "(") {
                    if (depth == 0) {
                        prefix = substr(line, 1, i - 1)
                        if (match(prefix, /(~?[a-zA-Z_][a-zA-Z0-9_]*)(::(~?[a-zA-Z_][a-zA-Z0-9_]*))*[[:space:]]*$/)) {
                            token = substr(prefix, RSTART, RLENGTH)
                            sub(/[[:space:]]+$/, "", token)
                            base = token
                            sub(/^.*::/, "", base)
                            if (!is_keyword(base)) name = token
                        }
                    }
                    depth++
                } else if (c == ")" && depth > 0) {
                    depth--
                }
            }
            return name
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
                        if (line ~ /(^|[^a-zA-Z_])assert[[:space:]]*\(/) n_asserts++
                        if (line ~ /VMAF_ASSERT_DEBUG[[:space:]]*\(/) n_asserts++
                        if (depth <= 0) {
                            printf "%s:%d %s %d %d\n", FILE, start_line, fname, 0, n_asserts
                            inside = 0
                            depth = 0
                            n_asserts = 0
                        }
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
    echo "assertion-density: ${total_asserts} asserts across ${total_funcs} project functions (avg ${avg})"
fi

if [ "$fail" -gt 0 ]; then
    echo "FAIL: ${fail} project functions ≥${MIN_LINES} lines have zero asserts" >&2
    exit 1
fi

echo "PASS: every project function ≥${MIN_LINES} lines has ≥1 assert"
