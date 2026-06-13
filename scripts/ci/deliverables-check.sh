#!/usr/bin/env bash
# scripts/ci/deliverables-check.sh — local + CI gate for ADR-0108
# six-deliverable checklist + ticked-file-references coherence.
#
# Single source of truth: previously this logic was inlined in
# .github/workflows/rule-enforcement.yml; the workflow now calls
# this script so a developer can run the same check locally before
# `gh pr create` (saving a 60-second CI round-trip on the typical
# "I forgot to untick the box for the opt-out" mistake).
#
# Usage:
#   scripts/ci/deliverables-check.sh
#       Reads PR body from $PR_BODY env var or stdin.
#       Diff is computed from $BASE_SHA..$HEAD_SHA env vars,
#       or falls back to $(git merge-base origin/master HEAD)..HEAD.
#
#   PR_BODY="$(gh pr view 260 --json body -q .body)" \
#       scripts/ci/deliverables-check.sh
#
#   gh pr view 260 --json body -q .body \
#       | scripts/ci/deliverables-check.sh
#
#   make pr-check PR=260
#       Wrapper target — see Makefile.
#
# Exits 0 on PASS, 1 on any deliverable miss or file-reference
# mismatch. Prints `::error` lines in GitHub Actions format so the
# workflow output stays unchanged when run from CI.

set -euo pipefail

# ---------- 1. Locate PR body ----------

if [ -n "${PR_BODY:-}" ]; then
  body_src="env"
elif [ ! -t 0 ]; then
  PR_BODY="$(cat)"
  body_src="stdin"
else
  echo "deliverables-check: no PR body supplied." >&2
  echo "  Set PR_BODY env var, OR pipe body on stdin, OR run via" >&2
  echo "  'make pr-check PR=<num>' which fetches it via gh." >&2
  exit 2
fi

# Strip markdown emphasis/code characters (backticks, asterisks,
# underscores, backslashes) before grepping: the PR template wraps
# labels like `AGENTS.md` in backticks, which would otherwise insert
# characters between tokens and break the `- [x].*<item>` regex.
# Backslashes appear when `gh pr create` is invoked with a heredoc-
# quoted body — the shell escapes embedded backticks and the
# leading backslash survives into the body, breaking the regex
# with spurious backslash-space separators between tokens.
tmp_body="$(mktemp)"
trap 'rm -f "$tmp_body" "${tmp_diff:-}"' EXIT
# shellcheck disable=SC1003  # the trailing \\ is a tr-recognised escape for a literal backslash
printf '%s' "${PR_BODY}" | tr -d '`*_\\' >"$tmp_body"

# ---------- 2. Locate diff base ----------

if [ -n "${BASE_SHA:-}" ] && [ -n "${HEAD_SHA:-}" ]; then
  diff_base="${BASE_SHA}"
  diff_head="${HEAD_SHA}"
  diff_src="env (BASE_SHA..HEAD_SHA)"
else
  if ! git rev-parse --verify origin/master >/dev/null 2>&1; then
    echo "deliverables-check: origin/master not found; run 'git fetch origin master' first." >&2
    exit 2
  fi
  diff_base="$(git merge-base origin/master HEAD)"
  diff_head="HEAD"
  diff_src="auto (merge-base origin/master..HEAD)"
fi

tmp_diff="$(mktemp)"
git diff --name-only "${diff_base}..${diff_head}" >"$tmp_diff"

echo "deliverables-check: PR body from ${body_src}, diff from ${diff_src}"
echo ""

# ---------- 2b. Anti-pattern detection (prose bullet form) ----------
#
# Agents occasionally write deliverable lines as prose bullets:
#   "- Research digest: docs/research/0435-foo.md"
# instead of the required checkbox form:
#   "- [x] **Research digest** — docs/research/0435-foo.md"
#
# This does NOT trigger a failure on its own — the six-deliverable parse
# below is authoritative. The warning here surfaces the probable root cause
# before the error line so contributors (and agents) understand why the gate
# is failing, rather than seeing only "Research digest is neither ticked nor
# opted-out" with no hint about the format mismatch.
#
# The patterns mirror the six deliverable names exactly (case-insensitive).
_prose_re='^[[:space:]]*[-*][[:space:]]+(Research digest|Decision matrix|AGENTS\.md invariant|Reproducer|CHANGELOG fragment|Rebase note)[[:space:]]*:'
if grep -qiEm1 "${_prose_re}" "${tmp_body}"; then
  echo "::warning title=ADR-0108 prose-bullet format::One or more deliverables appear to be in prose bullet"
  echo "  format (e.g. '- Research digest: docs/research/...'). The parser requires"
  echo "  the checkbox form: '- [x] **Research digest** — docs/research/NNNN-*.md'"
  echo "  or the opt-out form: '- [ ] **Research digest** — no digest needed: REASON'."
  echo "  See docs/development/pr-body-sentinel-guide.md for the exact syntax."
  echo ""
fi

# ---------- 3. Six-deliverable parse ----------

# The six deliverables, as named in .github/PULL_REQUEST_TEMPLATE.md
# under the "Deep-dive deliverables" heading.
items=(
  "Research digest"
  "Decision matrix"
  "AGENTS.md invariant note"
  "Reproducer / smoke-test command"
  "CHANGELOG fragment"
  "Rebase note"
)

fail=0
for item in "${items[@]}"; do
  # Look for either a ticked checkbox mentioning the item, or
  # an "opt-out" line per ADR-0108. Checkboxes use `- [x]`.
  # Opt-outs take the form `no <thing> needed: <reason>` or
  # `no rebase impact: <reason>` per the template.
  if grep -qiE -- "- \[x\].*${item}" "$tmp_body"; then
    echo "OK (ticked): ${item}"
    continue
  fi
  if grep -qiE "no .*(needed|impact|alternatives|rebase-sensitive)" "$tmp_body"; then
    case "${item}" in
      "Research digest") key="digest" ;;
      "Decision matrix") key="alternatives" ;;
      "AGENTS.md invariant note") key="rebase-sensitive|AGENTS" ;;
      "Reproducer / smoke-test command") key="reproducer|smoke" ;;
      "CHANGELOG fragment") key="changelog" ;;
      "Rebase note") key="rebase" ;;
    esac
    if grep -qiE "no .*(${key})" "$tmp_body"; then
      echo "OK (opt-out): ${item}"
      continue
    fi
  fi
  echo "::error title=ADR-0108 missing deliverable::${item} is neither ticked nor opted-out in the PR description."
  fail=1
done

if [ "$fail" -ne 0 ]; then
  echo ""
  echo "See ADR-0108 (docs/adr/0108-deep-dive-deliverables-rule.md)"
  echo "and .github/PULL_REQUEST_TEMPLATE.md for the six-deliverable"
  echo "checklist and the opt-out syntax."
  exit 1
fi

# ---------- 4. Ticked-file-references coherence ----------

echo ""
echo "deliverables-check: verifying ticked file-referencing items appear in diff..."

# Research digest — if ticked, expect docs/research/NNNN-*.md
if grep -qiE -- '- \[x\].*Research digest' "$tmp_body"; then
  if ! grep -qE '^docs/research/[0-9]+-' "$tmp_diff"; then
    echo "::error title=ADR-0108 research digest::Checkbox ticked but no docs/research/NNNN-*.md added in this PR."
    echo "  Hint: untick the box AND write 'no digest needed: <reason>' for the opt-out."
    fail=1
  fi
fi

# CHANGELOG entry — if ticked, expect either a new fragment file
# under changelog.d/<section>/ (preferred, ADR-0221) OR a hand
# edit to CHANGELOG.md itself (legacy fallback during migration).
if grep -qiE -- '- \[x\].*CHANGELOG' "$tmp_body"; then
  if ! grep -qE '^(CHANGELOG\.md|changelog\.d/[^/]+/.*\.md)$' "$tmp_diff"; then
    echo "::error title=ADR-0108 CHANGELOG::Checkbox ticked but neither CHANGELOG.md nor a new changelog.d/<section>/*.md fragment is in this PR (see ADR-0221)."
    fail=1
  fi
fi

# Rebase note — if ticked, expect docs/rebase-notes.md in diff
if grep -qiE -- '- \[x\].*Rebase note' "$tmp_body"; then
  if ! grep -qE '^docs/rebase-notes\.md$' "$tmp_diff"; then
    echo "::error title=ADR-0108 rebase note::Checkbox ticked but docs/rebase-notes.md is not in this PR."
    echo "  Hint: untick the box AND write 'no rebase impact: <reason>' for the opt-out."
    fail=1
  fi
fi

if [ "$fail" -eq 0 ]; then
  echo "deliverables-check: PASS — all six deliverables accounted for and every ticked file reference appears in the diff."
fi

exit "$fail"
