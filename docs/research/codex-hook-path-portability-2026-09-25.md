# Codex hook-path portability audit (2026-09-25)

## Question

Why did every repository-local Codex hook stop executing after the active fork
moved from the retired `vmaf` checkout to `VMAFx/vmafx`, and what command form
survives linked worktrees and future checkout moves?

## Evidence

- Commit `5730fca22` first tracked `.codex/hooks.json` with all seven commands
  bound to `/home/kilian/dev/vmaf/.codex/hooks/`. That directory no longer
  exists; the active checkout is resolved by `git rev-parse --show-toplevel`
  after clearing inherited repository-local Git variables.
- Codex CLI 0.145.0 reports the `hooks` feature as stable and consumes the
  tracked JSON shape already present in this repository.
- The seven target scripts remain tracked with Git mode `100755` under
  `.codex/hooks/`; only command resolution was broken.
- A regression test written before the fix failed on every exact mapping. The
  safe pre-tool script itself succeeded when invoked through the proposed
  repository-root command from `core/src/`.

The defect was therefore configuration path capture, not a missing script,
permission bit, hook event, or Codex feature flag. A JSON parser could not catch
it because the stale absolute strings were valid JSON and valid shell tokens.

## Alternatives considered

| Option | Result |
| --- | --- |
| Replace the retired path with the current absolute checkout | Rejected: it breaks again on another clone, user account, or linked worktree. |
| Use `.codex/hooks/<script>.sh` directly | Rejected: hook launch directories are not a stable repository-root contract. |
| Clear repository-local Git variables, then resolve `git rev-parse --show-toplevel` inside a quoted command | Selected: Git supplies the active main or linked-worktree root, inherited hook context cannot redirect it, and quoting preserves spaces. |
| Copy hooks into a user-global directory | Rejected: it separates executable state from the reviewed repository revision. |

No new ADR is needed: this is a one-way correction of a broken repository path,
not a new architectural or policy choice.

## Regression boundary

`scripts/ci/tests/test_codex_hook_config.py` fails closed on:

- any change to the seven event, matcher, type, or script mappings;
- duplicate or extra hook entries;
- anything other than the quoted Git-root command form;
- missing targets or a tracked mode other than `100755`; and
- failure to execute the safe pre-tool hook from a nested directory while a
  misleading inherited `GIT_WORK_TREE` points at that directory.

The dedicated `test-codex-hook-config` pre-commit/pre-push entry runs in the
required Pre-Commit CI job through `pre-commit --all-files`. Session-start is
not executed by the unit test because it may fetch `upstream`; the contract
instead verifies its exact path and executable mode without a network side
effect.

## Scope

The fix changes repository-local Codex configuration, its regression gate, and
operator documentation only. It does not change libvmaf code, numerical output,
models, training, benchmarking, dependency versions, public C/CLI surfaces, or
the FFmpeg patch stack. Netflix golden assertions are untouched.
