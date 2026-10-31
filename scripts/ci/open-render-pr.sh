#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Open or refresh the render-only pull request (ADR-3295, ADR-3373).
#
# Run by .github/workflows/docs-render-pr.yml after
# `check_render_on_master.py --open-after-hours` decided a render is owed long
# enough. Renders at the checked-out master tip, commits the rendered paths only
# as the release-bot App with a matching sign-off, force-pushes RENDER_BRANCH,
# opens the pull request when none is open and arms auto-merge, so the forge
# merges it once every required check is green and it is up to date.
#
# An open pull request on RENDER_BRANCH is only considered this workflow's PR
# when it is same-repository and authored by the App (app/${APP_SLUG} or
# ${APP_SLUG}[bot]), ignoring foreign or fork branches of the same name.
#
# A branch whose parent is already the master tip is left alone: re-pushing it
# would only restart its checks, but auto-merge is armed idempotently. With
# RENDER_DUE=false (master's render is current, for instance after a render
# landed by hand) an open render pull request of this workflow is closed instead.
# Inputs (environment): GH_TOKEN (App installation token), APP_SLUG,
# GITHUB_REPOSITORY, RENDER_BRANCH, RENDER_DUE, RUNNER_TEMP, OWED_SINCE.
set -euo pipefail

: "${GH_TOKEN:?}" "${APP_SLUG:?}" "${GITHUB_REPOSITORY:?}" "${RENDER_BRANCH:?}"
: "${RENDER_DUE:?}" "${RUNNER_TEMP:?}" "${OWED_SINCE:=}"
rendered=(CHANGELOG.md docs/adr/README.md docs/adr/titles.md docs/research/titles.md
  docs/adr/by-tag docs/rebase-notes.md)

find_open_pr() {
  gh pr list --repo "$GITHUB_REPOSITORY" --head "$RENDER_BRANCH" \
    --state open --json number,author,isCrossRepository \
    --jq "[.[] | select((.author.login == \"app/${APP_SLUG}\" or .author.login == \"${APP_SLUG}[bot]\") and .isCrossRepository == false)][0].number // empty"
}

arm_auto_merge() {
  local pr="$1"
  gh pr merge --repo "$GITHUB_REPOSITORY" --auto --squash "$pr" --body-file "$merge_body"
}

base="$(git rev-parse HEAD)"
open_pr="$(find_open_pr)"
if [[ "$RENDER_DUE" != true ]]; then
  if [[ -n "$open_pr" ]]; then
    gh pr close --repo "$GITHUB_REPOSITORY" --delete-branch "$open_pr" \
      --comment "Master's render is current at ${base:0:12}; this render is no longer owed."
    echo "closed render pull request #$open_pr: master's render is current"
  fi
  exit 0
fi

bot="${APP_SLUG}[bot]"
bot_id="$(gh api "users/$bot" --jq .id)"
email="${bot_id}+${bot}@users.noreply.github.com"
merge_body="$RUNNER_TEMP/render-merge-body.txt"
printf '%s\n\nSigned-off-by: %s <%s>\n' \
  "Render-only commit (ADR-3295, ADR-3373): make docs-render at ${base:0:12}." \
  "$bot" "$email" >"$merge_body"

parent=""
if [[ -n "$open_pr" ]]; then
  parent="$(gh api "repos/$GITHUB_REPOSITORY/branches/$RENDER_BRANCH" \
    --jq '.commit.parents[0].sha')"
fi
if [[ "$parent" == "$base" && -n "$open_pr" ]]; then
  arm_auto_merge "$open_pr"
  echo "render pull request #$open_pr is already rendered at ${base:0:12}"
  exit 0
fi

make -s docs-render
if [[ -z "$(git status --porcelain --untracked-files=all -- "${rendered[@]}")" ]]; then
  echo "make docs-render changed nothing at ${base:0:12}"
  exit 0
fi
stray="$(git status --porcelain --untracked-files=all -- . "${rendered[@]/#/:!}")"
if [[ -n "$stray" ]]; then
  echo "make docs-render wrote outside the rendered paths:" >&2
  echo "$stray" >&2
  exit 1
fi

msg="$RUNNER_TEMP/render-commit.txt"
printf 'chore(docs): render generated changelog and ADR index\n\n%s\n\nSigned-off-by: %s <%s>\n' \
  "Render-only commit (ADR-3295, ADR-3373): make docs-render at ${base:0:12}." \
  "$bot" "$email" >"$msg"
git switch -q -C "$RENDER_BRANCH"
git add -- "${rendered[@]}"
git -c user.name="$bot" -c user.email="$email" commit -q -F "$msg"
git push -q --force \
  "https://x-access-token:${GH_TOKEN}@github.com/${GITHUB_REPOSITORY}.git" \
  "HEAD:refs/heads/$RENDER_BRANCH"

if [[ -z "$open_pr" ]]; then
  body="$RUNNER_TEMP/render-body.md"
  {
    echo "Render-only pull request (ADR-3295): \`make docs-render\` output from the fragments on master ${base:0:12}, nothing edited by hand."
    echo
    echo "Opened by the scheduled render workflow (ADR-3373) because master has owed this render since ${OWED_SINCE:0:12}. It is re-rendered when master moves and merges through auto-merge once every required check is green."
    echo
    echo "Reproducer: \`make docs-render-check\` exits 0 on the head; \`python3 scripts/ci/check_render_on_master.py\` reports the render at HEAD current."
    echo
    echo "no state delta: render-only, no bug opened or closed."
  } >"$body"
  gh pr create --repo "$GITHUB_REPOSITORY" --base master --head "$RENDER_BRANCH" \
    --title "chore(docs): render generated changelog and ADR index" --body-file "$body"
  open_pr="$(find_open_pr)"
fi
arm_auto_merge "$open_pr"
echo "render pull request #$open_pr rendered at ${base:0:12}, auto-merge armed"
