<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1351: Move the praetor engine pin: re-record engine-measured debt and adopt its documentation gate

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: ci, governance, standards

## Context

[ADR-1249](1249-praetor-governance-adoption.md) adopted praetor as a ratchet: `.standards-baseline.json` may only shrink. The standards gate pins the engine by commit (`PRAETOR_REF` in `.github/workflows/standards-gate.yml`), because the HISS count depends on both the engine build and the tree.

This move takes the pin from `f41e74d` to praetor main `6c772713a133`, 163 commits later, installed from the remote module proxy like the gate does. The newest praetor commit when the move was prepared, `7f526eff9b32`, fails its own `Platform Neutrality (Linux)` and `(macOS)` checks. Its parent `6c772713a133` passes all 13 of its checks, so the pin is that parent. The move earlier proposed `25451d8` and was rebased onto the current master and re-run at the new ref.

On the same tree (this branch rebased onto master `c2af744`), `f41e74d` records 134 entries against the 182 on master (no growth) and `6c772713a133` records 378. None of the 244 added fingerprints comes from a code change and none is removed. An earlier probe on the master of 2026-09-30 (185 entries, 431 at `25451d8`) installed the engine at each responsible praetor commit and at its parent and recorded the same tree with both, which attributes every one of them. The new pin adds the same four kinds on today's tree (HISS-04 is 140 instead of 142 because master refactored two of those functions since), and the sets differ by exactly those fingerprints:

| Rule | Language | New entries | Praetor commit |
|---|---|---:|---|
| HISS-07 process exit from library code | Python 51, Go 5, Rust 5 | 61 | `d7a3778` (#455), abort policy |
| HISS-04 function length | Python | 140 | `025bbc6` (#460), a continuation line no longer closes a Python function early |
| HISS-01 recursion | Python | 36 | `025bbc6` (#460), Python recursion detection |
| HISS-02 call without a deadline | Go | 7 | `53e7594` (#510), HISS-02 I/O deadlines |

By rule, the baseline goes from 12 HISS-01, 8 HISS-02, 161 HISS-04 and 1 HISS-07 entries on master to 47, 15, 254 and 62. The engine refuses to record growth unless `baseline --record` gets `--allow-increase` and a `--reason`. The 244 are listed with file and line in a local report, `praetor-6c772713-new-hiss-findings.csv`, for the standards batches: the Go `os.Exit` calls in `cmd/vmafx-tune/cmd/root.go`, the Rust `process::abort` calls in `bindings/rust/vmafx-sys/src/safe.rs` and `bindings/rust/vmafx/src/context.rs`, and the seven Go calls without a deadline are real findings, not scanner artefacts.

The new engine also brings praetor's documentation gate for the `docs:seo-portal` facet. It adds a figure engine under `tools/figures/`, a `docs-figures` target and a `.gitattributes` block. Praetor fixed the three limits that kept the gate from passing here ([#532](https://github.com/cordanaLLM/praetor/issues/532), [#533](https://github.com/cordanaLLM/praetor/issues/533), [#534](https://github.com/cordanaLLM/praetor/issues/534), closed by praetor PR #543). Adoption no longer aborts at 4,096 directory entries ([#535](https://github.com/cordanaLLM/praetor/issues/535)), and `devcontainer --force` keeps the recorded base image ([#536](https://github.com/cordanaLLM/praetor/issues/536)).

Breaking praetor commits between the old pin and the new one were read for what they ask of this repository. Seven of them predate `25451d8` and were reviewed for the earlier proposal; none needs a further change here:

- [#631](https://github.com/cordanaLLM/praetor/pull/631) fails `compile-context --verify` and audit while Git does not ignore `.workingdir/evidence/`. The `/.workingdir/` rule in `.gitignore` already covers it, and `AGENTS.md` passes the new write-path lint.
- [#634](https://github.com/cordanaLLM/praetor/pull/634) stops `adopt` from replacing a hand-written `lefthook.yml`. This repository's file delegates to the pre-commit framework (ADR-1249), so adoption keeps it and lists the generated jobs it lacks.
- [#628](https://github.com/cordanaLLM/praetor/pull/628) and [#624](https://github.com/cordanaLLM/praetor/pull/624) change which required contexts the ruleset names. `.github/rulesets/main.json` is re-rendered by `sync` at the pin, and nothing here runs `sync --remote`.
- [#636](https://github.com/cordanaLLM/praetor/pull/636) fails audit on subdirectories under `.agents/agents`, which holds flat files only. It also replaces `scripts/docs_drift.py`, which this repository never used.
- [#629](https://github.com/cordanaLLM/praetor/pull/629) makes `bump audit` exit non-zero on a failed report, and [#623](https://github.com/cordanaLLM/praetor/pull/623) refuses `build.mjs sources --docs` without `--config` where both site configurations exist. Nothing here calls `bump audit`, and the root holds only `mkdocs.yml`.

Five more arrived after `25451d8`:

- [#639](https://github.com/cordanaLLM/praetor/pull/639) makes `audit` read the live Actions permissions and the last workflow runs from GitHub. `.standards.yaml` declares no `overrides.actions`, so nothing is compared, and the run report is informational (it lists `nightly.yml` as failing). The read takes about 40 seconds here against 2 seconds with `--offline`, so `scripts/git-hooks/hiss-audit.sh` passes `--offline` in the pre-commit and pre-push hooks when the engine has the flag; CI keeps the read.
- [#664](https://github.com/cordanaLLM/praetor/pull/664) makes audit fail a documentation-family file that Git ignores and does not track. Every managed file here is tracked.
- [#672](https://github.com/cordanaLLM/praetor/pull/672) adds JavaScript, TypeScript and Svelte scanners. They add no entry to the baseline; the audit names the languages it covers and prints the rest on an `[UNSCANNED]` line (shell 180 files, CUDA 3, JavaScript 2, Lua 2, PowerShell 2).
- [#680](https://github.com/cordanaLLM/praetor/pull/680) makes `adopt` exit non-zero when `compile-context --verify` rejects the result, and `adopt --force` project the agent context through one path. Adoption here is run in a throwaway copy (see Decision), and `compile-context --verify` passes.
- [#688](https://github.com/cordanaLLM/praetor/pull/688) changes the caveman clarity floor and splits the touched-file count in audit output. It changes no verdict on this tree.

## Decision

A praetor pin move may re-record the baseline upward with `--allow-increase` and a reason, under two conditions. The old engine must re-record no growth on the same tree. Every added fingerprint must trace to an engine change, not to a code change. Growth caused by code stays forbidden, as ADR-1249 states. The same PR regenerates every praetor-managed file with praetor's own code: `profile set`, `compile-context`, and the files `adopt --force` writes in a throwaway copy of the tree, which are copied back only where audit reports them stale. Praetor defects found during the move are filed on cordanaLLM/praetor, not declined in `.standards.yaml`. Where a defect blocks a step, the PR applies the output praetor would write and states how it verified that output.

For this move that means:

- The documentation gate's `documentation` block in `.standards.yaml` raises `max_files` to 8192 and `max_file_bytes` to 4 MiB, praetor's ceiling. It style-excludes the generated ADR index, the numbered ADR index fragments and `testdata/` fixtures. Those are the files the repository's own markdownlint hook in `.pre-commit-config.yaml` already skips. Every other finding is fixed in the source.
- The Makefile's `# BEGIN praetor documentation gate` block (`docs-lint`, `docs-figures`, `verify-all`) is praetor's own `adopt.DocumentationMakefileBlock()` text. Adoption of the earlier pin refused to write it because of the computed targets (`$(BUILD_DIR):` and four more, [#537](https://github.com/cordanaLLM/praetor/issues/537), still open), and GNU Make reports `No rule to make target 'docs-figures'` without the block. With the block in place, `adopt --force --dry-run` at the new pin reports the gate as already attached.
- The repository's `dist/` ignore rule hid praetor's committed figure player under `tools/figures/dist/`. `.gitignore` re-includes that one directory, because audit fails when those files are missing from a checkout ([#591](https://github.com/cordanaLLM/praetor/issues/591)).
- The repository's black, ruff and markdownlint pre-commit hooks skip `tools/figures/`. Praetor locks those files byte for byte, and black and ruff would rewrite `mkdocs_hook.py` ([#578](https://github.com/cordanaLLM/praetor/issues/578)).
- This amends [ADR-1277](1277-workingdir-contract-cleanup.md). ADR-1277 keeps the retired `.workingdir2/` visible to Git. Praetor's audit requires Git to ignore it, and the audit also requires the managed tail block to end `.gitignore` and to hold `/.workingdir2/`. The locked docs gate names the path too ([#641](https://github.com/cordanaLLM/praetor/issues/641)). `scripts/ci/check-local-data-contract.sh` therefore accepts the ignore rule only when it is that line inside praetor's block. It skips `tools/markdownlint/` when it scans for references, and it still fails when a `.workingdir2` directory exists locally. The exception goes once praetor lets a repository choose its private roots.
- `adopt --force` also registers `praetorctl hook <client> pre-tool` in `.claude/settings.json`, `.codex/hooks.json` and `.gemini/settings.json`. The move leaves those three files unchanged: they alter every agent session's tool hooks and need their own decision. Audit does not verify them. The text register block states that a registered dispatch hook denies a subagent brief without `task:` only where the hook is registered, so the compiled context stays true without it.
- `adopt --force` also replaces the repository's `AGENTS.md` harness with praetor's generic one, whose invariant table marks every rule as not enforced and advisory. The move keeps the repository's own table; `compile-context` and `compile-context --verify` pass on it, and only the register block is rendered.
- Text register. `.standards.yaml` declares every surface: `forge: social`, `docs: docs`, and `agent`, `context`, `ledger`, `hooks`, `prompts` and `mcp` as `internal` (the `caveman` form). Unset surfaces already resolve that way, so the declaration changes no rendered text and no verdict; it states the intent where a reader of the manifest looks. The engine gates what it can read as a file: the root `AGENTS.md`, the compiled vendor files, the canonical personas under `.agents/agents/` and the `register.sources` strings in `.paperclip/harness.json`. It does not read the 70 nested `AGENTS.md` files, the skills, agents and workflows under `.claude/`, or `.workingdir` ledger text, and it has no ratchet for them. `praetorctl caveman check --kind=context` fails 39 of the nested files (79 findings, mostly article density) and passes the 28 skills, 12 agents and 12 personas; the per-file counts are in a local report, `caveman-findings.csv`. Converting the nested files is separate work, and nothing blocks on it. The checker panics on a line that holds a lone `'` token (a possessive after a code span, for example), which hits `cmd/vmafx-mcp/AGENTS.md`; that is a praetor defect, reported with its reproducer.
- Hooks. `scripts/git-hooks/hiss-audit.sh` passes `--offline` to `audit` when the engine lists the flag, and the pre-push `audit` job runs the same wrapper. A hook run no longer waits about 40 seconds on the forge. An engine without the flag is run as before. CI keeps the forge read.
- The documentation gate's own lock is clean at the new pin: `npm audit --package-lock-only` on `tools/markdownlint/` reports no vulnerability, so no Dependency Review exception is needed.
- One master file broke the gate and is fixed here: `changelog.d/fixed/sycl-rc3-parity.md` started with a `### Fixed` heading, which the changelog renderer adds itself, and failed MD022, MD032 and MD041. The heading is removed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix the 244 findings before moving the pin | The baseline never grows | Weeks of refactoring AI scripts, CLIs and bindings, unrelated to governance; the engine stays stale meanwhile | Blocks every praetor update behind unrelated refactoring |
| Stay on `f41e74d` | No churn | The gate, the devcontainer and the managed files drift further from praetor; later moves get larger | The maintainer asked for the pin to move |
| Decline the new gates, or drop the `docs:seo-portal` facet | The move lands green | Hides praetor defects and weakens the declared policy | Rejected by the maintainer: declining is not the task |
| Style-exclude the model cards and `docs/backends/` too | Fewer edits | Hides 75 findings in hand-written pages that readers see | Only generated, partial and fixture Markdown is excluded |
| Rewrite the Makefile's computed targets as literal paths so `adopt` accepts the file | `adopt` writes the block itself | Duplicates `BUILD_DIR`, `DEBUG_DIR` and the venv paths as literals and changes the build Makefile for a praetor limit | The block text is praetor's own, and GNU Make confirms it collides with nothing |
| Keep ADR-1277's contract check unchanged | The retired root stays visible to `git status` | The required contract check and praetor's audit cannot both pass on any tree | Blocks the move until #641 is fixed |
| Drop ADR-1277's visibility rule entirely | Smallest script change | Any other rule could hide a stale `.workingdir2/` again | The exception names praetor's single audit-verified line |
| Take `adopt --force` output wholesale | One command, no judgement | Replaces the repository's invariant table with a generic one that marks every rule advisory, adds session-wide tool hooks and fails on `.gemini/settings.json` while the local exclude file hides it | The files that matter are the ones audit reports stale |
| Pin the head `7f526eff9b32` | Newest fixes | Its own `Platform Neutrality (Linux)` and `(macOS)` checks fail | The newest green ancestor is `6c772713a133` |
| Convert the 39 failing nested `AGENTS.md` files in this PR | Register compliance everywhere | Large unrelated rewrite of maintainer text; the gate does not ask for it | Separate lanes, with the per-file counts as input |
| Declare only the three default surfaces | Smaller diff | Unset surfaces resolve to `internal` silently; the manifest does not say so | The declaration is free and keeps the intent visible |
| Let the hooks read the forge | No change to the hook scripts | About 40 s per commit and push | `--offline` loses only a report; the declared policy compares nothing |
| Record the measured debt with `--allow-increase` (chosen) | The engine is current; the baseline states the real debt; the ratchet resumes at 378 | The recorded count and the README figure rise | — |

## Consequences

- **Positive**: The gate runs the current engine. The baseline reflects debt that already existed, so the ratchet protects against more of it from now on. `make docs-lint` and `make verify-all` pass in a Linux container with Node.js 24.
- **Negative**: The recorded count rises from 182 on master to 378. `make verify-all` now needs Node.js 24 and npm registry access for `docs-lint`. The rendered `.github/rulesets/main.json` targets `master` (`repository.default_branch`). It lists every required context praetor derives from the workflows and asks for two approvals and signed commits. It is praetor's template; the live GitHub ruleset is unchanged, because nothing runs `sync --remote`. Praetor's ignore rule hides a stale `.workingdir2/` from `git status`; the contract check's existence test still fails on one.
- **Neutral / follow-ups**: Every hook engine on a workstation has to move with the merge. The two engines do not read each other's trees, so a branch at the old pin fails the new engine's `compile-context --verify`, `audit` and `hiss coverage --verify`, and the new tree fails the old engine's and its `flavor audit`; `dedupe scan .` passes both ways. `docs/rebase-notes.md` is 2.9 MB of the 4 MiB ceiling and has to be split before it reaches it. GitHub's renderer splits a `|` inside a code span where the documentation site's Python-Markdown does not, so about 94 rows of the excluded `docs/adr/README.md` render with shifted cells on GitHub only. A later `adopt` will register the agent pre-tool hooks again unless someone decides for or against them.

## Supply-chain impact

- **New dependencies**: `tools/figures/` vendors interfig (MIT, Vectorize AI) and a player bundle with React, react-dom and scheduler (MIT). `REUSE.toml` labels both after the whole-tree table. `tools/markdownlint/package.json` adds js-yaml 5.2.2 and micromatch 4.0.8 (MIT), both already in its lock through markdownlint-cli2. All are dev-only and never reach libvmaf or its binaries.
- **Build-time fetches**: `make docs-lint` runs `npm ci` from `tools/markdownlint/package-lock.json` into a temporary directory. The devcontainer vendors the praetor `6c772713a133` source in five base64 parts.
- **Known advisories in a locked file**: none at this pin. The earlier pin's lock carried `smol-toml@1.7.0` (GHSA-7w5x-hrqm-74c2, high), `js-yaml@5.2.2` and `markdown-it@14.3.0` ([cordanaLLM/praetor#643](https://github.com/cordanaLLM/praetor/issues/643)); praetor's PR #651 moved them to `smol-toml@1.8.0`, `js-yaml@5.4.1` and `markdown-it@15.0.1`, and `T-PRAETOR-DOCS-GATE-LOCK-ADVISORIES-2026-09-30` is closed.

## References

- req: "praetor should have moved as well"
- req: "praetor upstream moved as well"
- req: "it lives remote"
- req: "declining everything is not the task"
- req: "if its a praetor issue, then open [...] issues on remote (praetor)"
- [ADR-1249](1249-praetor-governance-adoption.md): praetor adoption and the ratchet
- [CI guide: Praetor documentation gate](../development/ci.md#praetor-documentation-gate)
- Praetor issues [#532](https://github.com/cordanaLLM/praetor/issues/532) to [#537](https://github.com/cordanaLLM/praetor/issues/537), [#578](https://github.com/cordanaLLM/praetor/issues/578), [#591](https://github.com/cordanaLLM/praetor/issues/591), [#641](https://github.com/cordanaLLM/praetor/issues/641) and [#71](https://github.com/cordanaLLM/praetor/issues/71)
- [ADR-1277](1277-workingdir-contract-cleanup.md): private state, corpora and tracked evidence (amended here)
- Praetor [text register guide](https://github.com/cordanaLLM/praetor/blob/6c772713a133/docs/guides/text-register.md) and [PR #651](https://github.com/cordanaLLM/praetor/pull/651), at the pinned commit
- Task brief (paraphrased): move the pin to current praetor main and adopt the text-register rules, under which agent-only text is written in the terse `caveman` register
