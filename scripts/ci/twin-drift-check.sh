#!/usr/bin/env bash
# .c/.cpp twin-drift + stale-source-reference gate (ADR-1135).
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# Policy: two mechanically-decidable predicates, both blocking.
#
#   (a) Every .c/.cpp twin pair (same directory, same stem — e.g.
#       core/src/log.c + core/src/log.cpp) must have BOTH sides compiled by
#       at least one build file. A side no build file names is "dead": a fix
#       lands on the live twin and never reaches the dead one, or vice versa
#       (output.c vs output.cpp drifted for three months before anyone
#       noticed). Known dead sides are listed WITH a reason each in
#       scripts/ci/twin-drift-allowlist.txt; the gate fails on stale rows
#       (file gone, side now compiled, or no longer a twin) so the allowlist
#       cannot rot silently.
#
#   (b) Every source-file literal in a meson.build / setup.py / *.pyx must
#       resolve to a file in the tree. The mem.c -> mem.cpp and dict.c ->
#       dict.cpp renames broke the Cython extension and the libFuzzer
#       harnesses for two months because those build files are only
#       configured by nightly / opt-in lanes, so nothing on the PR path
#       noticed the stale paths. There is deliberately NO allowlist for (b):
#       fix the reference. A single line may opt out with an inline
#       `twin-drift-ignore: <reason>` comment (ADR-0278 style — the reason
#       is mandatory) for constructs the parser cannot model.
#
# Scope: source extensions c cpp cc cxx cu hip m mm metal pyx (compiled
# translation units, not headers — header literals in meson.build are
# dominated by cc.has_header() probes of system headers and would drown
# the gate in false positives). Build files: every tracked meson.build,
# setup.py and *.pyx.
#
# Resolution rules (see docs/development/ci.md for the contributor view):
#   - 'path/with/slash.c'           -> relative to the build file's directory
#   - 'bare.c'                      -> relative to the build file's directory
#   - some_dir + 'bare.c'           -> some_dir = './x/' assignment in the
#                                      same file resolves it exactly; a
#                                      prefix that is not a literal directory
#                                      (loop variables like _m + '_parity.c')
#                                      falls back to a suffix search over
#                                      the tracked-file list (reported NOTE)
#   - os.path.join("..", "x", "y.c") -> joined; identifiers resolve through
#                                      the same assignment table (setup.py)
#   - output: 'generated.c'          -> skipped (custom_target / configure_file
#                                      outputs do not exist in the tree)
#   - '@PLAINNAME@.c'                -> skipped (meson substitution)
#
# Exit 0 on pass, 1 on any FAIL line. Pure git + awk; no build required.

set -euo pipefail

ALLOWLIST="${TWIN_DRIFT_ALLOWLIST:-scripts/ci/twin-drift-allowlist.txt}"

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

git ls-files >"$tmpdir/tracked"
grep -E '(^|/)meson\.build$|(^|/)setup\.py$|\.pyx$' "$tmpdir/tracked" \
  >"$tmpdir/buildfiles" || true

if [ ! -s "$tmpdir/buildfiles" ]; then
  echo "twin-drift: no build files (meson.build / setup.py / *.pyx) found; skipping"
  exit 0
fi

n_buildfiles="$(wc -l <"$tmpdir/buildfiles")"
echo "twin-drift: scanning ${n_buildfiles} build files (meson.build / setup.py / *.pyx)"

fail=0
n_exact=0
n_search=0
n_ignored=0
: >"$tmpdir/refs" # resolved references: <path>\t<buildfile>

# ---------------------------------------------------------------------------
# Reference extractor. Invoked once per build file with the file passed
# TWICE: pass 1 (FNR == NR) collects `ident = '<literal>'` and
# `IDENT = os.path.join(<literals>)` assignments; pass 2 emits one record per
# source literal:  KIND \t LINE \t VALUE \t LITERAL
#   KIND = exact   VALUE = normalised repo-relative path
#   KIND = search  VALUE = suffix to look up in the tracked-file list
#   KIND = ignore  VALUE = the twin-drift-ignore reason
# POSIX awk only (mawk on Ubuntu runners): no gensub, no length(array).
# ---------------------------------------------------------------------------
read -r -d '' AWK_PROG <<'EOF' || true
function strip_comment(s,    i, c, q, out) {
    q = ""; out = ""
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (q != "") { if (c == q) q = ""; out = out c; continue }
        if (c == "'" || c == "\"") { q = c; out = out c; continue }
        if (c == "#") break
        out = out c
    }
    return out
}

# Collapse "." / ".." / "//" — literals are joined onto the build-file
# directory before normalisation so ".." never has to climb above the repo
# root for an in-tree file.
function norm(p,    n, parts, i, k, stack, out) {
    n = split(p, parts, "/")
    k = 0
    for (i = 1; i <= n; i++) {
        if (parts[i] == "" || parts[i] == ".") continue
        if (parts[i] == "..") {
            if (k > 0 && stack[k] != "..") { k--; continue }
            stack[++k] = ".."
            continue
        }
        stack[++k] = parts[i]
    }
    out = ""
    for (i = 1; i <= k; i++) out = (i == 1) ? stack[i] : out "/" stack[i]
    return out
}

function is_src(lit) {
    return lit ~ /\.(c|cpp|cc|cxx|cu|hip|m|mm|metal|pyx)$/
}

function trim(s) {
    sub(/^[[:space:]]+/, "", s); sub(/[[:space:]]+$/, "", s)
    return s
}

function unquote(s) {
    if (s ~ /^'[^']*'$/ || s ~ /^"[^"]*"$/) return substr(s, 2, length(s) - 2)
    return s
}

function is_quoted(s) {
    return s ~ /^'[^']*'$/ || s ~ /^"[^"]*"$/
}

function is_ident(s) {
    return s ~ /^[A-Za-z_][A-Za-z0-9_]*$/
}

# Identifier immediately before a trailing "+" in s ("" when absent).
function tail_ident(s,    t) {
    if (s !~ /[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\+[[:space:]]*$/) return ""
    t = s
    sub(/[[:space:]]*\+[[:space:]]*$/, "", t)
    sub(/.*[^A-Za-z0-9_]/, "", t)
    return t
}

# os.path.join(<args>) -> joined path. Every arg must be a literal or an
# identifier known from pass 1; otherwise returns "" and sets join_tail to
# the trailing literal run (used for the suffix-search fallback).
function join_args(inner,    n, a, i, x, out, ok) {
    n = split(inner, a, ",")
    out = ""; ok = 1; join_tail = ""
    for (i = 1; i <= n; i++) {
        x = trim(a[i])
        if (x == "") continue
        if (is_quoted(x)) {
            x = unquote(x)
            join_tail = (join_tail == "") ? x : join_tail "/" x
        } else if (is_ident(x) && (x in dirs)) {
            x = dirs[x]
            join_tail = ""
        } else {
            ok = 0
            join_tail = ""
            continue
        }
        out = (out == "") ? x : out "/" x
    }
    return ok ? out : ""
}

function emit(kind, value, lit) {
    printf "%s\t%d\t%s\t%s\n", kind, FNR, value, lit
}

function classify(lit, pre, first,    tailf, id) {
    if (!is_src(lit)) return
    if (lit ~ /@/) return          # meson @PLAINNAME@ / @BASENAME@ substitution
    if (lit ~ /^\//) return        # absolute path (toolchain-provided)
    tailf = pre
    sub(/.*,/, "", tailf)          # keyword args are comma separated
    if (tailf ~ /output[[:space:]]*:/) return   # generated file
    id = tail_ident(pre)
    if (id == "" && first && pre ~ /^[[:space:]]*$/) id = carry
    if (id != "") {
        if ((id in dirs) && (dirs[id] ~ /\/$/ || dirs[id] == "." || dirs[id] == "")) {
            emit("exact", norm(DIR "/" dirs[id] "/" lit), lit)
        } else {
            emit("search", lit, lit " (prefix `" id "` is not a literal directory)")
        }
        return
    }
    emit("exact", norm(DIR "/" lit), lit)
}

# ---- pass 1: assignment table --------------------------------------------
FNR == NR {
    s = strip_comment($0)
    if (s ~ /^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*['"][^'"]*['"][[:space:]]*$/) {
        id = s; sub(/^[[:space:]]*/, "", id); sub(/[[:space:]]*=.*$/, "", id)
        v = s; sub(/^[^'"]*['"]/, "", v); sub(/['"][[:space:]]*$/, "", v)
        dirs[id] = v
    } else if (s ~ /^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*os\.path\.join\([^()]*\)[[:space:]]*$/) {
        id = s; sub(/^[[:space:]]*/, "", id); sub(/[[:space:]]*=.*$/, "", id)
        v = s; sub(/^[^(]*\(/, "", v); sub(/\)[[:space:]]*$/, "", v)
        v = join_args(v)
        if (v != "") dirs[id] = v
    }
    next
}

# ---- pass 2: reference extraction ----------------------------------------
{
    raw = $0
    if (raw ~ /twin-drift-ignore:[[:space:]]*[^[:space:]]/) {
        reason = raw
        sub(/.*twin-drift-ignore:[[:space:]]*/, "", reason)
        emit("ignore", trim(reason), "")
        carry = ""
        next
    }
    s = strip_comment(raw)

    # os.path.join(...) segments are handled as a unit, then blanked out so
    # the literal loop below does not see their pieces a second time.
    while (match(s, /os\.path\.join\([^()]*\)/)) {
        seg = substr(s, RSTART, RLENGTH)
        inner = substr(seg, 14, length(seg) - 14)
        joined = join_args(inner)
        if (joined != "" && is_src(joined)) {
            emit("exact", norm(DIR "/" joined), joined)
        } else if (joined == "" && join_tail != "" && is_src(join_tail)) {
            emit("search", join_tail, join_tail " (os.path.join has a non-literal component)")
        }
        s = substr(s, 1, RSTART - 1) " " substr(s, RSTART + RLENGTH)
    }

    pre = ""; rest = s; first = 1
    while (match(rest, /'[^']*'|"[^"]*"/)) {
        lit = substr(rest, RSTART + 1, RLENGTH - 2)
        pre = pre substr(rest, 1, RSTART - 1)
        rest = substr(rest, RSTART + RLENGTH)
        classify(lit, pre, first)
        first = 0
        pre = pre "'" lit "'"
    }
    carry = tail_ident(s)
}
EOF

# grep -E metacharacter escape for the suffix search.
re_escape() {
  printf '%s' "$1" | sed -e 's/[][\\.*^$+?(){}|]/\\&/g'
}

while IFS= read -r bf; do
  [ -f "$bf" ] || continue
  bdir="$(dirname "$bf")"
  awk -v DIR="$bdir" "$AWK_PROG" "$bf" "$bf" >"$tmpdir/recs"
  while IFS=$'	' read -r kind line value lit; do
    case "$kind" in
      exact)
        n_exact=$((n_exact + 1))
        if [ -f "$value" ]; then
          printf '%s\t%s\n' "$value" "$bf" >>"$tmpdir/refs"
        else
          echo "FAIL: stale source reference ${bf}:${line} '${lit}' -> ${value} does not exist" >&2
          fail=$((fail + 1))
        fi
        ;;
      search)
        n_search=$((n_search + 1))
        esc="$(re_escape "$value")"
        matches="$(grep -E -- "(^|/)[^/]*${esc}\$" "$tmpdir/tracked" || true)"
        if [ -z "$matches" ]; then
          echo "FAIL: unresolved source reference ${bf}:${line} ${lit} — no tracked file ends with '${value}'" >&2
          fail=$((fail + 1))
        else
          n_matches="$(printf '%s\n' "$matches" | wc -l)"
          echo "NOTE: ${bf}:${line} ${lit} resolved by suffix search -> ${n_matches} tracked file(s)"
          while IFS= read -r m; do
            printf '%s\t%s\n' "$m" "$bf" >>"$tmpdir/refs"
          done <<<"$matches"
        fi
        ;;
      ignore)
        n_ignored=$((n_ignored + 1))
        echo "NOTE: ${bf}:${line} skipped via twin-drift-ignore (${value})"
        ;;
    esac
  done <"$tmpdir/recs"
done <"$tmpdir/buildfiles"

echo "twin-drift: ${n_exact} exact + ${n_search} suffix-searched source references, ${n_ignored} ignored lines"

# ---------------------------------------------------------------------------
# (a) twin pairs
# ---------------------------------------------------------------------------
grep -E '\.c$' "$tmpdir/tracked" | sed 's/\.c$//' | sort >"$tmpdir/stems_c" || true
grep -E '\.cpp$' "$tmpdir/tracked" | sed 's/\.cpp$//' | sort >"$tmpdir/stems_cpp" || true
comm -12 "$tmpdir/stems_c" "$tmpdir/stems_cpp" >"$tmpdir/twins"
n_twins="$(wc -l <"$tmpdir/twins")"
echo "twin-drift: ${n_twins} .c/.cpp twin pairs"

# Allowlist: "<path> <reason...>" per line, '#' comments, blank lines ignored.
declare -A allow_reason=()
declare -A allow_used=()
if [ -f "$ALLOWLIST" ]; then
  while IFS= read -r al; do
    case "$al" in '' | '#'*) continue ;; esac
    al_path="${al%%[[:space:]]*}"
    al_reason="${al#"$al_path"}"
    al_reason="${al_reason#"${al_reason%%[![:space:]]*}"}"
    if [ -z "$al_reason" ]; then
      echo "FAIL: allowlist ${ALLOWLIST}: entry '${al_path}' has no reason (a reason is mandatory, ADR-0278 style)" >&2
      fail=$((fail + 1))
      continue
    fi
    allow_reason["$al_path"]="$al_reason"
  done <"$ALLOWLIST"
fi

is_test_buildfile() {
  case "$1" in
    test/* | tests/* | fuzz/* | */test/* | */tests/* | */fuzz/*) return 0 ;;
    *) return 1 ;;
  esac
}

n_dead=0
n_allowlisted=0
n_test_only=0
while IFS= read -r stem; do
  for side in "${stem}.c" "${stem}.cpp"; do
    refby="$(awk -F '\t' -v p="$side" '$1 == p { print $2 }' "$tmpdir/refs" | sort -u)"
    if [ -z "$refby" ]; then
      if [ -n "${allow_reason[$side]+x}" ]; then
        allow_used["$side"]=1
        n_allowlisted=$((n_allowlisted + 1))
        echo "ALLOWLISTED: dead twin side ${side} — ${allow_reason[$side]}"
      else
        n_dead=$((n_dead + 1))
        echo "FAIL: dead twin side ${side} is compiled by no build file and is not in ${ALLOWLIST}" >&2
        fail=$((fail + 1))
      fi
      continue
    fi
    # A test source being compiled only by test build files is the normal
    # state; the INFO line is for production sources whose one live compile
    # is a unit test (the drift-risk shape OPEN.md tracks).
    is_test_buildfile "$side" && continue
    prod=0
    while IFS= read -r rb; do
      is_test_buildfile "$rb" || prod=1
    done <<<"$refby"
    if [ "$prod" -eq 0 ]; then
      n_test_only=$((n_test_only + 1))
      echo "INFO: test-only twin side ${side} — compiled only by $(printf '%s\n' "$refby" | paste -sd ',' -)"
    fi
  done
done <"$tmpdir/twins"

# Stale allowlist rows: every entry must name a currently-dead twin side.
for al_path in "${!allow_reason[@]}"; do
  [ -n "${allow_used[$al_path]+x}" ] && continue
  if ! grep -qxF -- "$al_path" "$tmpdir/tracked"; then
    echo "FAIL: allowlist ${ALLOWLIST}: '${al_path}' is not a tracked file — remove the row" >&2
  elif ! grep -qxF -- "${al_path%.*}" "$tmpdir/twins"; then
    echo "FAIL: allowlist ${ALLOWLIST}: '${al_path}' is not part of a .c/.cpp twin pair — remove the row" >&2
  else
    echo "FAIL: allowlist ${ALLOWLIST}: '${al_path}' is now compiled by a build file — remove the row" >&2
  fi
  fail=$((fail + 1))
done

echo
echo "twin-drift: ${n_allowlisted} allowlisted dead side(s), ${n_test_only} test-only side(s), ${n_dead} unlisted dead side(s)"

if [ "$fail" -gt 0 ]; then
  echo "FAIL: ${fail} twin-drift finding(s) — see docs/development/ci.md#twin-drift-gate (ADR-1135)" >&2
  exit 1
fi

echo "PASS: every .c/.cpp twin side is compiled by a build file (or allowlisted with a reason) and every source reference resolves"
