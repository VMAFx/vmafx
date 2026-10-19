# DCO sign-off

Every commit in a pull request carries a `Signed-off-by:` line. The required
check **DCO Sign-off** fails the pull request when one does not.
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
```

Exit status: `0` every commit is signed off or exempt, `1` at least one is not
(each is listed with its subject), `2` the range could not be read, which is
never reported as a pass.

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
The check reads the pull request's commits, not `master`.
