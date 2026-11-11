# DCO sign-off

Every commit in a pull request carries a `Signed-off-by:` line. The required
check **DCO Sign-off** fails the pull request when one does not, or when the
commit or pull request body carries model or coding tool attribution.
[ADR-2462](../adr/2462-dco-sign-off-required.md) records why.

## What the sign-off means

By signing off you state, under the
[Developer Certificate of Origin 1.1](https://developercertificate.org/), that
you wrote the change or have the right to submit it under the licence of the
files it touches. VMAFx has no contributor licence agreement; the sign-off is
the whole of the paperwork. See [CONTRIBUTING.md](../../CONTRIBUTING.md) and
[ADR-1250](../adr/1250-eupl-fork-relicense.md) for the licences.

## Sign off

```bash
git commit -s -m "fix(scope): subject"          # a new commit
git commit --amend -s --no-edit                 # the last commit
git rebase --signoff origin/master              # every commit of the branch
git push --force-with-lease
```

`-s` appends `Signed-off-by: Your Name <you@example.org>` from `user.name` and
`user.email`. The address must be the one in the commit's author or committer
field; a sign-off for somebody else does not count. The line must sit in the
last block of the message, as git writes it.

## Check before you push

```bash
python3 scripts/ci/check-dco.py --base origin/master --head HEAD
python3 scripts/ci/check-dco.py --base origin/master --head HEAD --pr-body-file path/to/pr-body.md
```

For the pull request body, pass `--pr-body-file PATH` or set `PR_BODY` in the
environment. The file takes precedence when both are present; an empty or
absent body has no lines to scan. CI supplies `PR_BODY` from the pull request
event and reruns this check after body edits.

Exit status: `0` all applicable checks pass, `1` an attribution or sign-off
check fails, `2` the range or body file could not be read. Commit failures
list the commit subject; body failures give the line number.

## Attribution rules

Both pull request and landed modes reject model or coding tool names in commit
author and committer names or email addresses. Matching is case-insensitive
and uses the tokens in `BANNED_COAUTHOR_TOKENS` in `scripts/ci/check-dco.py`.
The `[bot]` trailer token is excluded from identity checks, so legitimate bot
authors remain valid. The same message-line rules reject `Co-authored-by:`
lines with a banned token, generated or created attribution footers, and the
robot emoji. Pull request mode applies those line rules to the body as well.

For a refused commit, remove attribution trailers or footers and re-author
the commit with your own identity. For a refused pull request body, edit its
text. Bot commit sign-off exemptions do not exempt attribution. Machine-generated
release pull requests and pull requests grandfathered by the cutoff keep their
existing exemptions.

## Exemptions

Three, all narrow:

- A pull request created before the rollout cutoff, the UTC timestamp in
  `scripts/ci/dco-cutoff.txt` (the date is also in
  [CONTRIBUTING.md](../../CONTRIBUTING.md)). It is grandfathered: its commits
  are not judged, however often it is updated. A pull request created at or
  after the cutoff is judged, and one for which GitHub gives no creation time
  is judged too.
- A commit written by a listed bot, in a pull request that GitHub reports as
  opened by that bot. The list is `BOT_LOGINS` in
  `scripts/ci/check-dco.py`: `renovate[bot]`, `dependabot[bot]` and
  `github-actions[bot]`. A commit a person pushes onto a bot branch, or a
  commit that only names a bot as its author inside a person's pull request,
  is not exempt.
- The machine-generated release pull request, detected by
  `scripts/ci/release-pr-exempt.sh`.

Renovate also signs off its own commits (`:gitSignOff` in `renovate.json`), so
the exemption is a second line, not the only one.

Verbatim upstream ports and merge commits are not special-cased: merge commits
are not judged, and a port is committed by a person who signs it off like any
other commit.

## Agents and the merge train

Automation that commits for the project signs off like a person: commit with
`-s`, and keep every `Signed-off-by:` trailer when a pull request is squashed.

The pull request check reads the pull request's commits, not `master`.

## On master

On `master`, every squash commit is validated by the workflow
`.github/workflows/landed-commit-messages.yml` using `check-dco.py --landed`
([ADR-3389](../adr/3389-landed-commit-message-gate.md)). This is not a required
pull request check: it runs on push to `master` as an alarm after landing. A
failure indicates an invalid squash body landed on `master`; published history
is not rewritten.

The `--landed` mode disables all exemptions (bot logins, `--release-pr`, and
the cutoff date). It refuses any non-merge commit where:

- A `Co-authored-by:` line names a bot or automated tool (such as Renovate or
  coding assistants).
- A message line contains machine-generated attribution (`generated by`,
  `created with`, or the robot emoji).
- The commit author or committer name/email contains a model or coding tool token.
- The commit lacks a `Signed-off-by:` trailer matching either the commit
  author or the landing identity (`--landing-identity`). The committer address
  is not accepted because GitHub is the committer on every squash merge.

The check examines non-merge commits in the range `BASE_SHA..HEAD_SHA`. If
the base SHA is all zeros (initial push) or not an ancestor-reachable commit,
it evaluates the head commit alone.

Run the check locally with:

```bash
python3 scripts/ci/check-dco.py --landed --landing-identity "Name <email>" --base <sha> --head <sha>
```
