<!-- markdownlint-disable MD013 MD060 -->
# 2041 — Praetor adoption: measured behaviour and the two exclusions

Measurements behind [ADR-1249](../adr/1249-praetor-governance-adoption.md), taken
2026-09-15 against `praetorctl v1.0.0` and `origin/master` at `78c9d2bfc`. Every
number here came from running the tool, not from reading its documentation.

## How the adoption was proved before it was committed

A throwaway clone at `~/dev/scratch/vmafx-adopt-sandbox` was built from
`origin/master` plus the candidate governance files, then adopted for real. This
surfaced three blocking defects in order, none of which are visible from a dry
run:

1. **Discovery bound.** `adopt` aborted in 8 ms with `verification discovery
   exceeds 4096 entries` and wrote nothing. The repository has 8,751 filesystem
   entries against a hard 4,096-entry cap on the engine's verification walk. Only
   a newer engine exposing `--verification-max-entries` can adopt a tree this
   size.
2. **Lockfile format.** `praetorctl audit` fails immediately with `lockfile
   digest must be sha256:<64 hex characters>: ""`. The 54-byte `.standards.lock`
   produced by the earlier scaffolding attempt is a retired placeholder; a live
   adoption builds a digest-pinned lock plus a repository-local
   `.config/archetypes/` policy catalog.
3. **Context budget.** `context composition cannot produce valid projections:
   target file CLAUDE.md exceeds max line budget (565 > 300)`. The transpiler
   emits the **full** `AGENTS.md` body into six vendor files under a hard
   300-line-per-target budget with no configuration knob.

With all three addressed the adoption completes at exit 0 — 28 files created, 34
reconciled — and `praetorctl audit` then passes every gate: 1,687 baselined
infractions, 19 touched files clean, contexts in sync, ruleset, labels, paperclip
harness, epic and hooks all verified.

## Debt baseline

| Rule | Count | What it counts |
| --- | --- | --- |
| HISS-04 | 901 | functions above the archetype's length limit |
| HISS-01 | 583 | `goto` (non-DAG control flow) |
| HISS-07 | 74 | unchecked error assignments |
| HISS-02 | 66 | unbounded loops |
| HISS-09 | 48 | unaudited `unsafe`, `eval`/`exec`, banned libc |
| HISS-08 | 15 | banned `sprintf` / `strcpy`, dynamic execution |
| **Total** | **1,687** | across C, Python, Go, C++, Rust, CUDA, HIP |

The baseline exists so legacy code still builds while new code is gated; the
audit's rule is that it may only ratchet down.

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
protection stays under the existing aggregator. The generated ruleset does derive
its required contexts from this repository's own workflows rather than a fixed
list, selecting `build`, `Gate — check label / trigger` and `Required Checks
Aggregator`.

## Licensing: measured, and why REUSE stays out of rc1

`reuse lint` over 8,256 files:

| Measure | Count |
| --- | --- |
| Files with copyright information | 2,064 |
| Files **missing** copyright information | 6,192 |
| Files **missing** licensing information | 6,876 |
| Licence texts referenced but absent from `LICENSES/` | 7 |
| Invalid licence in `LICENSES/` | 1 (`Apache-2.0-u2netp`) |

Five of the seven missing texts are added by this change. Two identifiers used
in-tree are **not valid SPDX identifiers** and are deliberately untouched,
because changing them asserts a licence:

- `BSD+Patent` — 1 file. The deprecated spelling of `BSD-2-Clause-Patent`.
- **`BSD-3-Clause-Plus-Patent` — 710 fork-added files** across
  `tools/vmaf-tune/`, `core/test/`, `scripts/`, `cmd/vmafx-*` and `pkg/`. No such
  SPDX identifier exists, while the root `LICENSE` declares
  `BSD+Patent` / `BSD-2-Clause-Patent`.

Annotating the remaining ~6,900 files also means classifying vendored
third-party trees (`core/src/mcp/3rdparty/cJSON`,
`compat/python-vmaf/matlab/matlabPyrTools`, `model/`), where a wrong blanket
annotation is a licensing misstatement. Both jobs are their own reviewed changes,
so no REUSE gate is added before 1.0.0.

## Generated markdown does not satisfy this repository's markdownlint

Praetor's generated artefacts tripped the `Markdown Lint` gate on MD013, MD022,
MD024, MD031 and MD032: the agent persona sources, the pre-migration epic
document and `.paperclip/rules.md`. They were fixed in place — the persona fix at
its source so all four vendor projections inherit it. A newer engine run will
reintroduce the violations, so the real fix belongs upstream: praetor's templates
should emit markdown that passes a standard profile.

## Pre-existing drift found while wiring this up, not fixed here

- `docs/adr/by-tag/` is out of sync with its generator on `master`, and the
  current generator no longer emits the `<!-- markdownlint-disable MD013 MD060 -->`
  header the 590 committed files carry. Regenerating would strip it from all of
  them and redden the markdown gate. The generator needs the header restored
  before anyone runs `--write`; this becomes a CI gate once #1425 lands, which
  adds both generators to `make docs-fragments-check`.
- The `mkdocs.yml` ADR-nav block is missing roughly 107 ADRs from 1115 onward.
  The strict build reports these at INFO, not WARNING, so it is not currently
  fatal.
