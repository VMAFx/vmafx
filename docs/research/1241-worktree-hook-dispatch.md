# Research-1241: Local hook wiring and worktree lifetime

On 2026-09-08 the shared hook directory was configured explicitly as
`/home/kilian/dev/vmaf/.git/hooks`. Its `pre-push` was a dangling link to
`.claude/worktrees/wf_0a2cf2bc-58a-1/scripts/git-hooks/pre-push`.
The pre-commit and commit-msg files were generated framework hooks;
pre-rebase linked to the main checkout source. The installer generated
absolute worktree links and did not install a framework push hook.

## Primary evidence

Inspected installed pre-commit 4.6.2 with `hook-impl --help`,
`install --help`, and `pre_commit.commands.hook_impl` /
`pre_commit.commands.install_uninstall` source on Python 3.14.7.
`hook-impl` consumes Git's pre-push stdin and forwards the selected refs
into the hook run namespace. It also runs an existing executable
`HOOK.legacy`. `install` refuses a configured core.hooksPath; installing
its environments with `install-hooks` has no such hooks-path mutation.
These observations support the regular dispatcher design in ADR-1241.

The configured pre-push checks were assertion density, twin drift,
mypy, patch replay, and PR-body lint. `pre-commit run --all-files` in
hosted CI does not install Git hooks or test push-stage invocation.
The custom push hook's early returns skipped its trailing MkDocs call.
Despite documentation claiming registration, MkDocs had no config entry.

## Reproduction and validation

Run `python3 scripts/githooks/tests/test_install.py`. The fixture uses
real Git and pre-commit with only local hooks and a local bare remote;
GitHub and MkDocs responses are controlled test doubles. It proves a
regular installed hook survives deletion of the installer worktree and
that commit, commit-message, and push failures block Git operations.
MkDocs failures still block first and draft pushes. Native mode runs the
framework's message/push stages; legacy hooks and the rebase guard remain
active. Unknown hooks are refused without changing installed hooks.
This tests dispatch and failure propagation, not the native VMAF build
or a full MkDocs render. The required Pre-Commit job runs this fixture.

## Separate documentation findings

Read-only checks at master `7bafbb8cc` found that ADR index concatenation
and changelog concatenation passed, but ADR tag and navigation generation
were stale. Neither latter check was wired into make or CI. MkDocs strict
validation permits some informational link categories by intentional
policy and does not prove generated index freshness. The metadata refresh
and generator contracts are tracked independently of the hook repair.
