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

For the pull request body, pass `--pr-body-file PATH` (alias `--pr-body PATH`).
The file takes precedence over the local `PR_BODY` environment fallback; an
empty or absent body has no lines to scan. In CI, `--pr-body-from-event` reads
`pull_request.body` from the JSON file at `GITHUB_EVENT_PATH`. A null body is
empty; an unreadable file, malformed JSON, or non-string body is an error.
The file and event flags are mutually exclusive. Body edits rerun the check.
The DCO workflow job passes the body through files to both the attribution
check and the squash-message check, avoiding process environment size limits.

Exit status: `0` all applicable checks pass, `1` an attribution or sign-off
check fails (including an input bound), `2` the range or body file could not be
read. Commit failures list the commit subject; body failures give the line
number.

## Attribution rules

Both pull request and landed modes reject model or coding tool names in commit
author and committer names or email addresses. Matching is case-insensitive
and uses the tokens in `BANNED_COAUTHOR_TOKENS` in `scripts/ci/check-dco.py`.
The `[bot]` trailer token is excluded from identity checks, so legitimate bot
authors remain valid. The same message-line rules reject `Co-authored-by:`
lines with a banned token, generated or created attribution footers, and the
robot emoji. Pull request mode applies those line rules to the body as well.

Before the line rules run, text is normalized with Unicode NFKC and leading
whitespace and format characters (category `Cf`) are removed. This includes
U+FEFF, U+200B through U+200D, and U+2060. The normalization applies to both
commit-message and PR-body lines. Ordinary human co-authors remain permitted.
Identity token matching remains case-insensitive substring matching.

The checker refuses inputs above a named bound with exit `1` and the bound
name; it never silently truncates. Limits apply to UTF-8 bytes and Unicode
`splitlines()` line counts, with exact-limit input accepted:

| Input | Byte limit | Line limit |
| --- | ---: | ---: |
| Commit message | `MAX_COMMIT_MESSAGE_BYTES` = 281,876 | `MAX_COMMIT_MESSAGE_LINES` = 5,836 |
| PR body | `MAX_PR_BODY_BYTES` = 235,864 | `MAX_PR_BODY_LINES` = 2,176 |

Commit limits use four times the measured maxima of the 6,290-commit master
snapshot. PR limits use four times the measured maxima of the 300 PR bodies
numbered 2906-3395; see the
[research digest](../research/3531-dco-attribution-scan.md) for the
corpus fingerprints and the reproduction command. File reads consume at most
the byte limit plus one byte. Event JSON has an additional
`MAX_PR_EVENT_BYTES` envelope: six times the body-byte limit plus 1 MiB for
metadata and JSON escapes. Commit output is captured one commit at a time
with `MAX_COMMIT_LOG_BYTES` (twice the message-byte limit plus 65,536 bytes
for metadata); exceeding this capture bound also refuses the input. Git
fields and records are NUL-delimited so embedded legacy separators cannot
move identity text into another field.

For a commit refused only because of an attribution trailer or footer, edit the
message with `git commit --amend`, which keeps the original author. If the
author identity is refused, use `--author="Name <email>"` on the refused commit
to correct that author, or `--reset-author` to assign authorship to the current
committer. If only the committer identity is refused, correct `user.name` and
`user.email`, then use `git commit --amend --no-edit -S` for the last commit:
amend retains the author and message while recording the current committer
identity and signing the replacement commit. The [`git-commit(1)`
manual](https://git-scm.com/docs/git-commit) documents these options and the
identity configuration; explicit author/committer settings or environment
variables override `user.name` and `user.email`. For a series,
`git rebase --force-rebase <base>` forces every rebased commit to be replayed,
including unchanged commits, as documented in the [`git-rebase(1)`
manual](https://git-scm.com/docs/git-rebase).
For a refused pull request body, edit its text. Bot commit
sign-off exemptions do not exempt attribution. Machine-generated release pull
requests and pull requests grandfathered by the cutoff keep their existing
exemptions.

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
