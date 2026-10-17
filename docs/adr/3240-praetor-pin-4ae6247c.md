<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3240: Move the praetor pin to 4ae6247c57d2, the engine the workstation hooks already run

- **Status**: Accepted
- **Date**: 2026-10-11
- **Deciders**: lusoris
- **Tags**: ci, governance, standards

## Context

Since 2026-10-11 no commit passes the local hooks on a tree at the pin `d141862c430b` ([ADR-3061](3061-praetor-pin-d141862c.md)). The hooks do not run the pinned engine. `lefthook.yml` and `scripts/git-hooks/hiss-audit.sh` take whichever `praetorctl` is first on `PATH`, and the workstation's copy follows praetor main: it was replaced 29 times between 2026-10-09 11:41 and 2026-10-11 12:12, each time with a build of another praetor commit, and is now `4ae6247c57d2`. CI is not affected, because the Standards gate builds the engine from `PRAETOR_REF`.

On a clean copy of master (`23ac10eee`) the engine at `4ae6247c57d2` stops both hook commands:

- `praetorctl compile-context --verify`: "target AGENTS.readonly.md missing or unreadable". The engine compiles a read-only projection of `AGENTS.md` since cordanaLLM/praetor#1168 and verifies it like the other compiled files.
- `praetorctl audit --offline`: "Documentation gate asset tools/markdownlint/package.json holds an earlier Praetor text; run 'praetorctl adopt' to refresh it". Audit stops at the first stale asset. `.github/workflows/praetor-docs.yml` and `.github/workflows/praetor-api.yml` fail the same check once the first is refreshed.

28 praetor commits lie between the two pins. Four of them changed text that `adopt` writes here without audit calling the old text stale: the register skills gained upstream credit and licence files (#1064) and list rules for `social-text` (#1222), the adopted hook script `.config/agent/hooks/block_evasion.py` lost four CodeQL findings (#1224), and the managed `.gitignore` block gained `/.standards/cache/` (#1094).

Measured with both engines, `audit --offline`:

- `d141862c430b` on clean master: 143 warnings (142 "Workflow triggers", one about cache-band markers in `AGENTS.md`).
- `4ae6247c57d2` on this tree: the same 143 warnings, line for line, and the audit passes. The recorded baseline does not change.

## Decision

Move `PRAETOR_REF` in `.github/workflows/standards-gate.yml` to `4ae6247c57d2fb06e33b7e94979411de5f4ed98a` under [ADR-1351](1351-praetor-engine-pin-move.md)'s conditions:

- `praetorctl adopt --lock-source-root=<praetor clone at the pin>` runs in a throwaway clone, without `--force`.
- The files the new engine refuses are copied back: `AGENTS.readonly.md` (new), `tools/markdownlint/package.json` with its lock file, `tools/markdownlint/verify.mjs`, `.github/workflows/praetor-docs.yml` and `.github/workflows/praetor-api.yml`.
- Three more groups are copied back although audit accepts their old text, by operator decision: the three register skills under `.agents/skills/` and `.claude/skills/` with their `LICENSE` files, `.config/agent/hooks/block_evasion.py`, and the `.gitignore` line.
- Not copied back: `.claude/settings.json` and `.codex/hooks.json`, where `adopt` adds a `PreToolUse` entry that calls `praetorctl hook <client> pre-tool`, and `.gemini/settings.json`. Agent settings change only on the operator's own decision, which is open.

`AGENTS.readonly.md` is compared byte for byte like the other compiled files, so it gets `text eol=lf` in `.gitattributes` and an entry in `scripts/ci/tests/test_praetor_hashed_files_lf.py`. Without the attribute that test fails for the file. As a new top-level repository file, it also joins `known_files` in `.github/ci-impact.json` so the CI planner classifies it without falling back to full mode.

## Alternatives considered

| Option | Why not |
| --- | --- |
| Keep the pin and put a `d141862c430b` engine first on `PATH` for hooks | Every session and every agent worktree would have to set it, and the workstation's copy changes again within hours. The pin would also stay 28 commits behind without a reason. |
| Copy back only what audit refuses | The skills would keep text adapted from MIT-licensed work without its licence file and credit line, and the hook script would keep four findings praetor has fixed. The next adopt writes all of it anyway. |
| Copy back the `PreToolUse` entries too | They make every tool call of an agent session start the engine on `PATH`. That is a change to the agents' permissions and to the cost of every call, and it would bind the sessions to the same unpinned engine that caused this move. |
| Adopt praetor's pinned-engine launcher in the same change | `adopt` wrote no launcher in the throwaway clone and left `lefthook.yml` as it is: a repository that keeps its own `lefthook.yml` gets none (cordanaLLM/praetor#1172, open). The launcher of cordanaLLM/praetor#1165 arrives with that fix and is adopted then, measured on its own. |

## Consequences

- Commits pass the hooks again on a tree at this pin with the workstation's engine. A branch moves by rebasing onto this change.
- The cause stays: the hooks run the engine on `PATH`. The next praetor commit that changes an adopted text or adds a compiled target refuses commits here again until the pin follows. `docs/state.md` carries this as `T-HOOKS-ENGINE-FROM-PATH-2026-10-11`. On praetor's side it is cordanaLLM/praetor#1172 (no launcher for a repository with its own `lefthook.yml`), #1173 (agent hooks) and #1194 (direct calls); #1172 is the one that lets this repository pin its hook engine.
- `AGENTS.readonly.md` is a seventh file compiled from `AGENTS.md`. It is the context for agents that run read-only: steps that change files, state or history are dropped, prohibitions kept. Edit `AGENTS.md` and run `praetorctl compile-context`, never the projection.
- `praetor-api.yml` and `praetor-docs.yml` also start on `merge_group`. This repository has no merge queue, so the trigger does not fire.
- The agent settings differ from what `adopt` would write until the `PreToolUse` decision is made. Audit does not check them.

## References

- Source: operator decisions of 2026-10-11, ledger ids `vmafx-adopt-lag` and `vmafx-adopt-extras` (paraphrased): move the pin together with the files the new engine requires, add the skills' licence files and credit lines, the hook script fixes and the ignore line, and leave the agent settings out.
- cordanaLLM/praetor#1168 (read-only projection), #1064 and #1222 (register skills), #1224 (hook script), #1198 (setup-node and js-yaml), #1165 (pinned-engine launcher), #1172, #1173 and #1194 (engine on `PATH`).
- [ADR-1351](1351-praetor-engine-pin-move.md), [ADR-3061](3061-praetor-pin-d141862c.md).
