#!/usr/bin/env bash
# scripts/ci/state-md-touch-check.sh — local + CI gate for ADR-0165
# bug-status-hygiene rule (CLAUDE.md §12 rule 13).
#
# Promotes the `docs/state.md` update discipline from reviewer-
# enforced to CI-enforced. The gate trips when a PR looks like it
# closes / opens / rules-out a bug AND the diff does not touch
# `docs/state.md` AND the PR body does not carry the explicit
# opt-out sentinel `no state delta: <REASON>`.
#
# The script intentionally has the same shape as
# scripts/ci/deliverables-check.sh (single-purpose, env-driven,
# stdin-friendly) so a developer can run it locally before
# `gh pr create`:
#
#   PR_TITLE="fix: foo segfault" \
#   PR_BODY="$(gh pr view 999 --json body -q .body)" \
#       scripts/ci/state-md-touch-check.sh
#
# Or via the workflow, where PR_TITLE / PR_BODY / BASE_SHA /
# HEAD_SHA are supplied by GitHub Actions.
#
# Exits 0 on PASS or "trigger conditions did not fire", 1 on a
# missing state.md update, 2 on bad invocation. Prints
# `::error` lines in GitHub Actions format on failure.

set -euo pipefail

# ---------- 1. Locate inputs ----------

if [ -z "${PR_TITLE:-}" ]; then
  echo "state-md-touch-check: \$PR_TITLE not set." >&2
  echo "  Set PR_TITLE (and PR_BODY) before invoking; the GitHub" >&2
  echo "  workflow does this from \${{ github.event.pull_request.* }}." >&2
  exit 2
fi

if [ -n "${PR_BODY:-}" ]; then
  body_src="env"
elif [ ! -t 0 ]; then
  PR_BODY="$(cat)"
  body_src="stdin"
else
  # Empty body is legal — a PR with no description simply cannot
  # carry an opt-out sentinel and falls through to the diff-only
  # check.
  PR_BODY=""
  body_src="empty"
fi

# Strip HTML comments first — the PR template parks the literal
# placeholder "no state delta: REASON" inside <!-- ... --> blocks
# as instructional text. Leaving it in would let an unedited
# template body satisfy the opt-out check on every PR. Then strip
# markdown emphasis / code characters (backticks, asterisks,
# backslashes) the same way deliverables-check.sh does.
tmp_body="$(mktemp)"
trap 'rm -f "$tmp_body" "${tmp_diff:-}"' EXIT
# shellcheck disable=SC1003  # the trailing \\ is a tr-recognised escape for a literal backslash
printf '%s' "${PR_BODY}" |
  python3 -c 'import re,sys; sys.stdout.write(re.sub(r"<!--.*?-->", "", sys.stdin.read(), flags=re.DOTALL))' |
  tr -d '`*\\' >"$tmp_body"

# ---------- 2. Locate diff base ----------

if [ -n "${BASE_SHA:-}" ] && [ -n "${HEAD_SHA:-}" ]; then
  diff_base="${BASE_SHA}"
  diff_head="${HEAD_SHA}"
  diff_src="env (BASE_SHA..HEAD_SHA)"
else
  if ! git rev-parse --verify origin/master >/dev/null 2>&1; then
    echo "state-md-touch-check: origin/master not found; run 'git fetch origin master' first." >&2
    exit 2
  fi
  diff_base="$(git merge-base origin/master HEAD)"
  diff_head="HEAD"
  diff_src="auto (merge-base origin/master..HEAD)"
fi

tmp_diff="$(mktemp)"
git diff --name-only "${diff_base}..${diff_head}" >"$tmp_diff"

echo "state-md-touch-check: PR body from ${body_src}, diff from ${diff_src}"

# ---------- 3. Trigger predicate ----------
#
# Any one of the following lights the gate:
#
#   1. PR title contains a Conventional-Commit `fix:` / `fix(scope):`
#      prefix (case-insensitive).
#   2. PR title contains the bare token `bug` (case-insensitive,
#      word-boundary-ish — accept "bug", "Bug", "BUG", "bug-fix"
#      but not "debug" / "subgraph").
#   3. PR title or body contains a `closes #N` / `fixes #N` /
#      `resolves #N` GitHub-issue reference (case-insensitive).
#   4. PR body has the `## Bug-status hygiene` template section
#      with the standard checkbox unchecked (`- [ ] ... docs/state.md`).
#
# The first three mirror Conventional Commits + GitHub's own
# auto-close keyword set; the fourth catches the case where the
# author copied the template but neither ticked the box nor wrote
# the opt-out.

trigger_reason=""

case "${PR_TITLE}" in
  [Ff][Ii][Xx]:* | [Ff][Ii][Xx]\(*)
    trigger_reason="title carries Conventional-Commit fix: prefix"
    ;;
esac

if [ -z "$trigger_reason" ]; then
  # Word-boundary check for "bug" — reject "debug", "subgraph",
  # accept "bug", "bug-fix", "Bug fix", "BUG-1234".
  if printf '%s' "${PR_TITLE}" | grep -qiE '(^|[^a-z])bug([^a-z]|$)'; then
    trigger_reason="title mentions 'bug'"
  fi
fi

if [ -z "$trigger_reason" ]; then
  if printf '%s\n%s' "${PR_TITLE}" "${PR_BODY}" |
    grep -qiE '(closes|fixes|resolves)[[:space:]]+#[0-9]+'; then
    trigger_reason="body or title carries a GitHub close-issue reference"
  fi
fi

if [ -z "$trigger_reason" ]; then
  # Template section with the box left unticked. Match on the
  # stripped body so backticks around `docs/state.md` don't break
  # the literal substring search.
  if grep -qE -- '- \[ \].*docs/state\.md' "$tmp_body"; then
    trigger_reason="PR body carries the Bug-status-hygiene checkbox UNCHECKED"
  fi
fi

if [ -z "$trigger_reason" ]; then
  echo "state-md-touch-check: trigger conditions did not fire — PASS (nothing to enforce)."
  exit 0
fi

echo "state-md-touch-check: triggered (${trigger_reason})."

# ---------- 4. Pass conditions ----------

# 4a. docs/state.md appears in the diff → check for placeholder refs first,
# then PASS if none were found.
if grep -qxF 'docs/state.md' "$tmp_diff"; then
  # ----- 4a-i. Placeholder-ref hardening (ADR-0334 status update 2026-05-09).
  #
  # PR #541's row audit found that the dominant staleness mode is NOT
  # missing-row but post-merge backfill drift: the closing PR's branch
  # writes "this PR" / "this commit" as the closer-PR placeholder, the
  # merge happens, and the placeholder never gets rewritten to the
  # merged numeric refs. The original gate only checks that the diff
  # *touches* state.md — it does not check that newly-added rows cite
  # a real merged PR or commit SHA.
  #
  # We REJECT inserted lines (lines starting with `+` in a unified
  # diff, but NOT the `+++ b/...` header) that contain any of:
  #
  #   - "this PR"            (case-insensitive — covers "this pr",
  #                          "(this PR)", "this PR (branch, date)")
  #   - "this commit"        (case-insensitive)
  #   - bare "TBD"           (case-insensitive, word-boundary)
  #   - the literal "<PR>"   (template placeholder)
  #   - the literal "#NNN"   (template placeholder; real PR refs use
  #                          digits)
  #
  # The canonical accept forms — explicitly NOT matched by the regex —
  # are `PR #N` (where N is one-or-more digits) and `commit \`<sha>\``.
  # Sample lines from PR #541's audit findings that this rejects:
  #
  #     | foo | this PR (fix/foo, 2026-05-08) | ... |
  #     | foo | closed by this PR             | ... |
  #     | foo | TBD                            | ... |
  #
  # And the corresponding accept forms (none match the regex):
  #
  #     | foo | PR #432                                   | ... |
  #     | foo | PR #511 / commit `f809ce09` (merged ...)  | ... |
  #
  # Bypass: standard CI exit-1 (the user can edit + push again).
  tmp_state_diff="$(mktemp)"
  # shellcheck disable=SC2064  # we want $tmp_state_diff resolved now
  trap "rm -f \"$tmp_body\" \"${tmp_diff:-}\" \"$tmp_state_diff\"" EXIT
  git diff -U0 "${diff_base}..${diff_head}" -- docs/state.md >"$tmp_state_diff"

  # Inserted-line predicate: starts with single `+`, not `+++`. Strip
  # the leading `+` so subsequent regex doesn't have to anchor around
  # diff metadata.
  inserted_lines="$(grep -E '^\+[^+]' "$tmp_state_diff" | sed 's/^+//' || true)"

  placeholder_hits=""
  if [ -n "$inserted_lines" ]; then
    # The five placeholder forms. Each printed prefixed with its
    # canonical-replacement hint so the failure message names the fix
    # next to the offence.
    if echo "$inserted_lines" | grep -inE '(^|[^a-z])this[[:space:]]+pr([^a-z]|$)' >/dev/null; then
      placeholder_hits="${placeholder_hits}$(echo "$inserted_lines" | grep -inE '(^|[^a-z])this[[:space:]]+pr([^a-z]|$)' | sed 's/^/  [this PR]   /')"$'\n'
    fi
    if echo "$inserted_lines" | grep -inE '(^|[^a-z])this[[:space:]]+commit([^a-z]|$)' >/dev/null; then
      placeholder_hits="${placeholder_hits}$(echo "$inserted_lines" | grep -inE '(^|[^a-z])this[[:space:]]+commit([^a-z]|$)' | sed 's/^/  [this commit]   /')"$'\n'
    fi
    if echo "$inserted_lines" | grep -inE '(^|[^A-Za-z])TBD([^A-Za-z]|$)' >/dev/null; then
      placeholder_hits="${placeholder_hits}$(echo "$inserted_lines" | grep -inE '(^|[^A-Za-z])TBD([^A-Za-z]|$)' | sed 's/^/  [TBD]   /')"$'\n'
    fi
    if echo "$inserted_lines" | grep -inF '<PR>' >/dev/null; then
      placeholder_hits="${placeholder_hits}$(echo "$inserted_lines" | grep -inF '<PR>' | sed 's/^/  [<PR>]   /')"$'\n'
    fi
    # `#NNN` literal — three capital N's; reject it as template
    # placeholder. Real PR refs use digits, e.g. `#432`.
    if echo "$inserted_lines" | grep -inF '#NNN' >/dev/null; then
      placeholder_hits="${placeholder_hits}$(echo "$inserted_lines" | grep -inF '#NNN' | sed 's/^/  [#NNN]   /')"$'\n'
    fi
  fi

  if [ -n "$placeholder_hits" ]; then
    cat <<EOF
::error title=ADR-0165 docs/state.md placeholder ref::Inserted lines in docs/state.md still carry a placeholder PR/commit reference. Per ADR-0334 (status update 2026-05-09), state.md rows must cite the merged numeric PR (e.g. \`PR #432\`) or commit SHA (e.g. \`commit \`f809ce09\`\`), not a "this PR" / "this commit" / "TBD" placeholder.

PR #541's row audit found that the dominant state.md staleness pattern
is post-merge backfill drift: a closing PR's branch writes "this PR"
as the placeholder, the merge happens, the placeholder never gets
rewritten. This gate prevents that drift mode at the CI boundary.

Offending lines:
${placeholder_hits}
Rewrite as \`PR #N (commit \\\`<sha>\\\`)\` before squash-merge.
For an in-flight PR whose number is not yet final, you can either:

  1. Land the row with a placeholder and push a follow-up commit
     rewriting it after \`gh pr create\` returns the number, OR
  2. Use \`PR #<this-pr-number>\` once GitHub has assigned it (the
     PR number is known the moment \`gh pr create\` exits).

Both paths satisfy this gate; "this PR" / "this commit" / "TBD" /
"<PR>" / "#NNN" do not.
EOF
    exit 1
  fi

  echo "state-md-touch-check: PASS — docs/state.md is in the diff (no placeholder refs in inserted lines)."
  exit 0
fi

# 4b. Explicit opt-out sentinel in the PR body. Format:
#       no state delta: <REASON>
# REASON must be non-empty AND not the literal placeholder
# "REASON" — that's template instructional text leaking through
# (the HTML-comment instances are already stripped above; this
# guards the example inside the checkbox row itself). We require
# at least one *lowercase* alphanumeric/punctuation token after
# the colon, which a copy-pasted "REASON" placeholder fails by
# being all-caps.
if grep -qiE 'no state delta:[[:space:]]*[^[:space:].]+' "$tmp_body" &&
  ! grep -qE 'no state delta:[[:space:]]*REASON([[:space:].]|$)' "$tmp_body"; then
  echo "state-md-touch-check: PASS — opt-out 'no state delta: ...' present."
  exit 0
fi

# ---------- 5. Fail ----------

cat <<'EOF'
::error title=ADR-0165 docs/state.md drift::This PR looks bug-shaped (fix: prefix, 'bug' in title, closes/fixes/resolves #N, or the Bug-status-hygiene checkbox is unchecked) but neither updates docs/state.md nor carries the 'no state delta: REASON' opt-out.

Per CLAUDE.md §12 rule 13 / ADR-0165, every PR that closes a bug,
opens a bug, or rules a Netflix upstream report not-affecting-the-
fork updates docs/state.md in the SAME PR. The update lands a row
in the appropriate section:

  - Open                     — known issue under investigation
  - Recently closed          — bug closed by this PR (or a recent one)
  - Confirmed not-affected   — Netflix issue ruled out for the fork
  - Deferred                 — explicitly punted to upstream / later

…and cross-links the ADR, the PR + commit, and the Netflix issue
(if any). State drift compounds across sessions; the rule trades a
30-second edit for hours of re-investigation cost.

Either:

  1. Add a row under the appropriate section in docs/state.md, OR
  2. Add 'no state delta: REASON' to the PR description (legitimate
     for pure feat / refactor / infra PRs with no bug-status impact).
EOF

exit 1
