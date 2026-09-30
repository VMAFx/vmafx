<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1351: Move the praetor engine pin: re-record engine-measured debt and adopt its documentation gate

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: ci, governance, standards

## Context

[ADR-1249](1249-praetor-governance-adoption.md) adopted praetor as a ratchet: `.standards-baseline.json` may only shrink. The standards gate pins the engine by commit (`PRAETOR_REF` in `.github/workflows/standards-gate.yml`), because the HISS count depends on both the engine build and the tree.

This move takes the pin from `f41e74d` to praetor main `25451d8`, 147 commits later, installed from the remote module proxy like the gate does. On the same tree (this branch rebased onto master `f5dff7b`), `f41e74d` re-records 185 entries with no growth and `25451d8` records 431. None of the 246 added fingerprints comes from a code change and none is removed. Installing the engine at each responsible praetor commit and at its parent, and recording the same tree with both, attributes every one of them:

| Rule | Language | New entries | Praetor commit | Count on the same tree |
|---|---|---:|---|---|
| HISS-07 process exit from library code | Python 51, Go 5, Rust 5 | 61 | `d7a3778` (#455), abort policy | 185 → 246 |
| HISS-04 function length | Python | 142 | `025bbc6` (#460), a continuation line no longer closes a Python function early | 246 → 424 |
| HISS-01 recursion | Python | 36 | `025bbc6` (#460), Python recursion detection | (same step) |
| HISS-02 call without a deadline | Go | 7 | `53e7594` (#510), HISS-02 I/O deadlines | 424 → 431 |

From `53e7594` to `25451d8` the count stays at 431. The engine refuses to record growth unless `baseline --record` gets `--allow-increase` and a `--reason`.

The new engine also brings praetor's documentation gate for the `docs:seo-portal` facet. It adds a figure engine under `tools/figures/`, a `docs-figures` target and a `.gitattributes` block. Praetor fixed the three limits that kept the gate from passing here ([#532](https://github.com/cordanaLLM/praetor/issues/532), [#533](https://github.com/cordanaLLM/praetor/issues/533), [#534](https://github.com/cordanaLLM/praetor/issues/534), closed by praetor PR #543). Adoption no longer aborts at 4,096 directory entries ([#535](https://github.com/cordanaLLM/praetor/issues/535)), and `devcontainer --force` keeps the recorded base image ([#536](https://github.com/cordanaLLM/praetor/issues/536)).

## Decision

A praetor pin move may re-record the baseline upward with `--allow-increase` and a reason, under two conditions. The old engine must re-record no growth on the same tree. Every added fingerprint must trace to an engine change, not to a code change. Growth caused by code stays forbidden, as ADR-1249 states. The same PR regenerates every praetor-managed file with praetor's own code: `adopt`, `sync`, `devcontainer --source-root`, `compile-context`. Praetor defects found during the move are filed on cordanaLLM/praetor, not declined in `.standards.yaml`. Where a defect blocks a step, the PR applies the output praetor would write and states how it verified that output.

For this move that means:

- The documentation gate's `documentation` block in `.standards.yaml` raises `max_files` to 8192 and `max_file_bytes` to 4 MiB, praetor's ceiling. It style-excludes the generated ADR index, the numbered ADR index fragments and `testdata/` fixtures. Those are the files the repository's own markdownlint hook in `.pre-commit-config.yaml` already skips. Every other finding is fixed in the source.
- `adopt` still refuses to add `docs-figures` to the Makefile block: the Makefile's computed targets (`$(BUILD_DIR):` and four more) are left to review ([#537](https://github.com/cordanaLLM/praetor/issues/537), still open). GNU Make reports `No rule to make target 'docs-figures'` for the Makefile without the block, so the refreshed block is praetor's own `adopt.DocumentationMakefileBlock()` text. Once it is in place, `adopt` completes.
- The repository's `dist/` ignore rule hid praetor's committed figure player under `tools/figures/dist/`. `.gitignore` re-includes that one directory, because audit fails when those files are missing from a checkout ([#591](https://github.com/cordanaLLM/praetor/issues/591)).
- The repository's black, ruff and markdownlint pre-commit hooks skip `tools/figures/`. Praetor locks those files byte for byte, and black and ruff would rewrite `mkdocs_hook.py` ([#578](https://github.com/cordanaLLM/praetor/issues/578)).
- This amends [ADR-1277](1277-workingdir-contract-cleanup.md). ADR-1277 keeps the retired `.workingdir2/` visible to Git. Praetor's audit requires Git to ignore it, and the audit also requires the managed tail block to end `.gitignore` and to hold `/.workingdir2/`. The locked docs gate names the path too ([#641](https://github.com/cordanaLLM/praetor/issues/641)). `scripts/ci/check-local-data-contract.sh` therefore accepts the ignore rule only when it is that line inside praetor's block. It skips `tools/markdownlint/` when it scans for references, and it still fails when a `.workingdir2` directory exists locally. The exception goes once praetor lets a repository choose its private roots.
- `adopt` also registers `praetorctl hook <client> pre-tool` in `.claude/settings.json`, `.codex/hooks.json` and `.gemini/settings.json`. The move leaves those three files unchanged: they alter every agent session's tool hooks and need their own decision. Audit does not verify them.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix the 246 findings before moving the pin | The baseline never grows | Weeks of refactoring AI scripts, CLIs and bindings, unrelated to governance; the engine stays stale meanwhile | Blocks every praetor update behind unrelated refactoring |
| Stay on `f41e74d` | No churn | The gate, the devcontainer and the managed files drift further from praetor; later moves get larger | The maintainer asked for the pin to move |
| Decline the new gates, or drop the `docs:seo-portal` facet | The move lands green | Hides praetor defects and weakens the declared policy | Rejected by the maintainer: declining is not the task |
| Style-exclude the model cards and `docs/backends/` too | Fewer edits | Hides 75 findings in hand-written pages that readers see | Only generated, partial and fixture Markdown is excluded |
| Rewrite the Makefile's computed targets as literal paths so `adopt` accepts the file | `adopt` writes the block itself | Duplicates `BUILD_DIR`, `DEBUG_DIR` and the venv paths as literals and changes the build Makefile for a praetor limit | The block text is praetor's own, and GNU Make confirms it collides with nothing |
| Keep ADR-1277's contract check unchanged | The retired root stays visible to `git status` | The required contract check and praetor's audit cannot both pass on any tree | Blocks the move until #641 is fixed |
| Drop ADR-1277's visibility rule entirely | Smallest script change | Any other rule could hide a stale `.workingdir2/` again | The exception names praetor's single audit-verified line |
| Record the measured debt with `--allow-increase` (chosen) | The engine is current; the baseline states the real debt; the ratchet resumes at 431 | The recorded count and the README figure rise | — |

## Consequences

- **Positive**: The gate runs the current engine. The baseline reflects debt that already existed, so the ratchet protects against more of it from now on. `make docs-lint` and `make verify-all` pass in a Linux container with Node.js 24.
- **Negative**: The recorded count rises from 185 to 431. `make verify-all` now needs Node.js 24 and npm registry access for `docs-lint`. The rendered `.github/rulesets/main.json` targets `master` (`repository.default_branch`). It lists every required context praetor derives from the workflows and asks for two approvals and signed commits. It is praetor's template; the live GitHub ruleset is unchanged, because nothing runs `sync --remote`. Praetor's ignore rule hides a stale `.workingdir2/` from `git status`; the contract check's existence test still fails on one.
- **Neutral / follow-ups**: `docs/rebase-notes.md` is 2.9 MB of the 4 MiB ceiling and has to be split before it reaches it. GitHub's renderer splits a `|` inside a code span where the documentation site's Python-Markdown does not, so about 94 rows of the excluded `docs/adr/README.md` render with shifted cells on GitHub only. A later `adopt` will register the agent pre-tool hooks again unless someone decides for or against them.

## Supply-chain impact

- **New dependencies**: `tools/figures/` vendors interfig (MIT, Vectorize AI) and a player bundle with React, react-dom and scheduler (MIT). `REUSE.toml` labels both after the whole-tree table. `tools/markdownlint/package.json` adds js-yaml 5.2.2 and micromatch 4.0.8 (MIT), both already in its lock through markdownlint-cli2. All are dev-only and never reach libvmaf or its binaries.
- **Build-time fetches**: `make docs-lint` runs `npm ci` from `tools/markdownlint/package-lock.json` into a temporary directory. The devcontainer vendors the praetor `25451d8` source in five base64 parts.

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
