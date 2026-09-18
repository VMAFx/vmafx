<!-- markdownlint-disable MD013 MD060 -->
# 2064 — Praetor adoption: measured behaviour and the two exclusions

Measurements behind [ADR-1249](../adr/1249-praetor-governance-adoption.md). The
first adoption was measured on 2026-09-15 against praetor `5a671ab` and
`origin/master` at `78c9d2bfc`. It was refreshed on 2026-09-18 against praetor
`e4b35cb` and `origin/master` at `4becc4072`; the
[refresh section](#refresh-onto-praetor-e4b35cb-2026-09-18) records what changed.
Every number here came from running the tool, not from reading its documentation.

## How the adoption was proved before it was committed

A throwaway clone was built from `origin/master` plus the candidate governance
files, then adopted for real. The first run surfaced three blocking defects in
order, none of which are visible from a dry run:

1. **Discovery bound.** `adopt` aborted in 8 ms with `verification discovery
   exceeds 4096 entries` and wrote nothing. The repository has 8,751 filesystem
   entries against a default 4,096-entry cap on the engine's verification walk.
   The engine now exposes `--verification-max-entries` (ceiling 200,000),
   `--verification-max-depth` and `--verification-max-files`, and the refresh ran
   with all three raised to their ceilings.
2. **Lockfile format.** `praetorctl audit` fails immediately with `lockfile
   digest must be sha256:<64 hex characters>: ""`. The 54-byte `.standards.lock`
   produced by the earlier scaffolding attempt is a retired placeholder; a live
   adoption builds a digest-pinned lock plus a repository-local
   `.config/archetypes/` policy catalog.
3. **Context budget.** `context composition cannot produce valid projections:
   target file CLAUDE.md exceeds max line budget (565 > 300)`. The transpiler
   emits the **full** `AGENTS.md` body into six vendor files under a hard
   300-line-per-target budget with no configuration knob.

With all three addressed the first adoption completed at exit 0 and
`praetorctl audit` passed every gate with 1,687 baselined infractions.

## Refresh onto praetor e4b35cb (2026-09-18)

The branch was rebased over 170 `master` commits and regenerated with a scratch
build of praetor `e4b35cb`, run in a clean clone so the working tree's build
directories could not inflate the scan. Five things changed.

### The engine now lints AGENTS.md

`compile-context --verify` and `audit` run the caveman lint over the whole
canonical `AGENTS.md`, including this repository's own part below the praetor
harness, with no opt-out (praetor ADR-0010). The prose version failed with 4.7
articles per 100 prose words against a limit of 2.0. `adopt --force` rewrote the
harness in the internal register, and the repository's part was rewritten by
hand. The gates for that rewrite, and their results:

| Check | Result |
| --- | --- |
| `praetorctl caveman check AGENTS.md` | pass: 791 prose words, 0.3 articles per 100 |
| `praetorctl caveman floor <prose> AGENTS.md` | pass: no lost code span, command, id, link, marker or directive |
| `markdownlint-cli2 AGENTS.md` | 0 issues |
| Largest compiled target | `.cursor/rules/hiss-invariants.mdc`, 263 lines of 300 |

The rewrite also absorbed the content that only the hand-written `CLAUDE.md`
carried (the source layout, the IDE note, `make preflight`, the banned-function
list and the golden-data test files), because `CLAUDE.md` is now compiled from
`AGENTS.md` and would otherwise have lost it. The hard rules that only
`CLAUDE.md` carried (no benchmark output commits, the per-session ADR index
check, the `docs/state.md` rule and the host-enforcement notes) moved to
[the hard-rules page](../development/agent-hard-rules.md) for the same reason.

### Persona projections are verified in both directions

The engine now rejects any file in `.claude/agents/`, `.codex/agents/`,
`.github/agents/` or `.gemini/agents/` that has no source in `.agents/agents/`.
The ten reviewer personas this repository keeps in `.claude/agents/` had none, so
each gained a byte-identical canonical copy in `.agents/agents/`, and
`compile-context` projected it to the other three vendor directories. The
`.claude/agents/` files themselves are unchanged; editing one now means editing
its `.agents/agents/` source and recompiling.

### Debt baseline

The rescan over the rebased tree records fewer infractions, because `master`
removed debt in the meantime:

| Rule | 2026-09-15 | 2026-09-18 | What it counts |
| --- | --- | --- | --- |
| HISS-04 | 901 | 889 | functions above the effective length limit |
| HISS-01 | 583 | 538 | `goto` (non-DAG control flow) |
| HISS-07 | 74 | 74 | unchecked error assignments |
| HISS-02 | 66 | 65 | unbounded loops |
| HISS-09 | 48 | 48 | unaudited `unsafe`, `eval`/`exec`, banned libc |
| HISS-08 | 15 | 13 | banned `sprintf` / `strcpy`, dynamic execution |
| **Total** | **1,687** | **1,627** | across C, Python, Go, C++, Rust, CUDA, HIP |

The baseline exists so legacy code still builds while new code is gated; the
audit's rule is that it may only ratchet down.

### The hook gate no longer asks who installed the hooks

At `e4b35cb`, `auditGitHooks` requires `lefthook.yml` at the root and, outside
CI, any file at `.git/hooks/pre-commit`. Under `CI=true` or `GITHUB_ACTIONS=true`
it skips the local check entirely, so the CI job no longer installs lefthook.

### Adoption defects the refresh hit

- **Editor scan bound.** `adopt` aborts at `synthesize editors: editor language
  scan exceeds file bound`. `internal/editor/capabilities.go` caps the walk at
  4,096 files and, unlike the verification walk, exposes no flag. The generation
  run declined the `editors` step through `adoption.decline`; that declaration was
  not committed, so the editor configurations from the first adoption stand.
- **Placeholder `ruff.toml`.** The `working-dir-and-flavor` step wrote a
  `ruff.toml` containing only a comment, and the adoption report did not list it.
  Ruff reads `ruff.toml` in preference to `pyproject.toml`, so committing it would
  silently discard this repository's `[tool.ruff]` configuration. It was not
  committed. `praetorctl flavor audit .` passes without it.
- **Generated markdown.** The regenerated personas and `.paperclip/rules.md`
  fail this repository's markdownlint profile again (MD013, MD022, MD031,
  MD032). Their content changes (`standardsctl` renamed to `praetorctl`, one new
  text-register rule) were applied to the lint-clean versions instead.
- **Harness spacing.** The generated harness renders
  ``Check first:`praetorctl caveman check AGENTS.md` `` without a space after the
  colon. It is left as generated because the harness is praetor's text.

## The HISS-04 limit is reported three different ways

| Source | `max_func_loc` |
| --- | --- |
| `.config/archetypes/native-gpu-systems.yaml` (the declared archetype) | 75 |
| `praetorctl audit` effective policy | 60, via `builtin:audit-compat-v1` |
| `praetorctl plan` | 100, the generic `DefaultPolicy` |

The audit is the gate, so 60 is the number that binds today. Worth an upstream
issue: `plan` should print what `audit` will enforce.

## Why `sync --remote` is not used here

The engine hardcodes the protected branch name: `ReconcileProtection(ctx,
"main", …)` in `cmd/standardsctl/sync.go` and `refs/heads/main` plus
`refs/heads/lts-*` in `internal/forge/ruleset.go`. This fork's default branch is
deliberately `master` ([ADR-0002](../adr/0002-merge-path-master-default.md)).
There is no `default_branch` key in `.standards.yaml` to override it, so the
generated `.github/rulesets/main.json` is kept as a local declaration and branch
protection stays under the existing aggregator. The generated ruleset derives its
required contexts from this repository's own workflows rather than a fixed list;
at the refresh it selects `build`, `Gate — check label / trigger`,
`Required Checks Aggregator`, `Scorecard PR Gate` and
`Standards & Invariant Verification Gate`.

## Licensing: measured, and why REUSE stays out of rc1

`reuse lint` over 8,256 files on 2026-09-15:

| Measure | Count |
| --- | --- |
| Files with copyright information | 2,064 |
| Files **missing** copyright information | 6,192 |
| Files **missing** licensing information | 6,876 |
| Licence texts referenced but absent from `LICENSES/` | 7 |
| Invalid licence in `LICENSES/` | 1 (`Apache-2.0-u2netp`) |

The identifier and licence-text findings have since been resolved on `master`,
not here: the EUPL relicensing ([ADR-1250](../adr/1250-eupl-fork-relicense.md))
and the residual SPDX correction
([ADR-1255](../adr/1255-spdx-residual-identifier-correction.md)) replaced every
`BSD-3-Clause-Plus-Patent` and `BSD+Patent` tag and added the missing texts to
`LICENSES/`, so this change no longer adds any. The copyright gap is untouched:
annotating the remaining files also means classifying vendored third-party trees
(`core/src/mcp/3rdparty/cJSON`, `compat/python-vmaf/matlab/matlabPyrTools`,
`model/`), where a wrong blanket annotation is a licensing misstatement. That is
its own reviewed change, so no REUSE gate is added before 1.0.0.

## Generated markdown does not satisfy this repository's markdownlint

Praetor's generated artefacts trip the `Markdown Lint` gate on MD013, MD022,
MD024, MD031 and MD032: the agent persona sources, the pre-migration epic
document and `.paperclip/rules.md`. They are fixed in place, the personas at their
`.agents/agents/` source so every vendor projection inherits the fix. Every
engine run reintroduces the violations, and the `e4b35cb` refresh did, so the
real fix belongs upstream: praetor's templates should emit markdown that passes a
standard profile.

## Pre-existing drift found during the first adoption

- `docs/adr/by-tag/` was out of sync with its generator, and the mkdocs ADR-nav
  block was missing roughly 107 ADRs. Both are now enforced on `master` by
  [ADR-1242](../adr/1242-generated-adr-freshness.md), which runs the generators'
  check modes in `make docs-fragments-check`, pre-commit and Docs CI.
