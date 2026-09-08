# ADR-1241: Keep Git hooks independent of installer worktrees

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: Kilian, Codex
- **Tags**: ci, docs, workspace, agents

## Context

The clone's shared pre-push hook pointed into a deleted agent worktree.
The installer created absolute source links, so deleting an installer
worktree silently removed enforcement. It also installed only framework
pre-commit and commit-msg hooks, leaving declared pre-push checks unused.
The custom PR-body hook returned early before its MkDocs call on first
pushes and drafts. Installed Git and pre-commit sources confirm these
are separate hook entry points and must receive Git's arguments and stdin.

## Decision

Install regular dispatchers for pre-commit, commit-msg, pre-push, and
pre-rebase. Resolve the active worktree at runtime, use the framework's
hook implementation for its stages, and register MkDocs independently in
the push configuration. Recognize only known managed hooks, preserve each
replacement under a unique backup, and refuse unknown custom hooks before
installation. Preserve ADR-0924's explicit native formatter choice while
activating message and push stages in both modes. Exercise installation
and real Git lifecycle failures in a disposable-repository CI fixture.

## Alternatives considered

| Option | Benefit | Cost or reason rejected |
| --- | --- | --- |
| Regular dispatcher copies | Survive removal; preserve active-tree rules | Reinstall to update dispatcher logic; chosen |
| Link into main checkout | Simple | Assumes that checkout remains at the same location |
| Framework installation alone | Standard stage handling | Does not install the source rebase guard; refuses configured hooksPath |
| Overwrite custom hooks | Fewer steps | Could remove contributor checks; rejected |

## Consequences

Installation requires the active Python environment's pre-commit package.
Push checks previously declared but unwired now run. Native mode continues
to have a reduced pre-commit check surface, documented explicitly.
MkDocs remains optional locally under ADR-0466; hosted Docs is required.
The fixture uses local repositories and no GitHub calls. Subprocess calls
use fixed argv and resolved executables; narrow scanner annotations explain
those calls where static analysis cannot follow the local helper arguments.
Existing installations must run `make install-hooks` after updating.

## References

- `req`: "if you find bugs/whatever, just fix them, its not out of scope".
- [ADR-0924](0924-native-pre-commit-hooks.md): native formatter opt-in.
- [ADR-0466](0466-mkdocs-strict-pre-push-hook.md): local docs gate.
- [Research-1241](../research/1241-worktree-hook-dispatch.md): observed
  wiring, installed framework source, and reproducer.
