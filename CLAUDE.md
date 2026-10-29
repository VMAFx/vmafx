<!-- markdownlint-disable MD013 -->
<!-- Compiled automatically by praetorctl compile-context from AGENTS.md. DO NOT EDIT DIRECTLY. -->

<!-- markdownlint-disable MD013 MD025 -->
# vmafx Agent Operating Harness

Before concluding any turn:

```bash
make verify-all
```

`make verify-all` = `praetorctl audit` + context verification + HISS evidence replay. All pass -> Ed25519 Exit-0 receipt. Fail -> SARIF diagnostic distillation (<= 1500 tokens).

## Core Directives & Invariants (Modernized NASA JPL Power-of-10)

| Invariant | Scope | NASA Rule | Enforcement Mechanism | Failure Action |
| :--- | :--- | :--- | :--- | :--- |
| **HISS-01** | Control Flow | Rule 1 | Recursion strictly prohibited; call graph must be DAG; zero `goto`. | Immediate build failure |
| **HISS-02** | Loops & I/O | Rule 2 | Scalar upper bound on all loops; explicit `context.Context` timeout on all I/O. | Semgrep / AST error |
| **HISS-03** | Memory | Rule 3 | Zero dynamic heap allocation (`malloc` / `free`) in hot simulation/tick loops. | Allocation audit sweep |
| **HISS-04** | Complexity | Rule 4 | Function length $\le 60$ LOC, McCabe Cyclomatic $\le 10$, Statements $\le 50$. | AST sweep blocker |
| **HISS-07** | Error Handling | Rule 7 | Zero `.unwrap()` / `.expect()`; all errors handled or wrapped with context. | Linter / Compiler error |
| **HISS-08** | Determinism | Rule 8 | Zero dynamic execution (`eval` / `exec`); zero banned unsafe libc (`gets` / `strcpy` / `sprintf`). | AST / Linter error |
| **HISS-09** | Reference Safety | Rule 9 | Mandatory `// SAFETY:` proofs for all pointer arithmetic and `unsafe` blocks. | AST check blocker |
| **HISS-10** | Warning Hygiene | Rule 10 | Zero-warning tolerance across compiler, linter, and format sweeps. | Exit code 1 |
| **HISS-11** | Hermetic Supply Chain | Fleet | Dependency inputs and images are pinned; release artifacts carry provenance and signatures. | Supply-chain gate |
| **HISS-14** | Append-Only ABI | Fleet | Public APIs evolve additively; breaking changes require `!` and a `Migration:` footer. | API / release gate |
| **HISS-15** | 3D Testing | Rule 5 | Positive, negative, and boundary tests mandatory for all public interfaces. | CI coverage gate |
| **HISS-16** | Context Integrity | Fleet | Single canonical `AGENTS.md`; vendor files compiled via `praetorctl compile-context`. | Pre-commit blocker |
| **HISS-17** | State Ledger | Fleet | Turn start reads bounded state/open tasks; turn end syncs private `.workingdir` state. | Pre-commit / CI gate |
| **HISS-18** | CI Efficiency | Fleet | Diff-aware classification skips heavy gates only with an explicit recorded reason. | CI routing gate |
| **HISS-19** | Reuse Before Writing | Fleet | One behavior, one implementation; extend or call existing capabilities. | Dedupe gate |
| **HISS-20** | Replayable Evidence | Fleet | Every scanner claim is replayed against positive, negative, and known-gap fixtures. | HISS evidence gate |
| **HISS-21** | Platform Neutrality | Fleet | Gates run on Linux, macOS, and Windows, or skip with a stated reason and alternate evidence. | Platform matrix gate |

## Operational Rules

1. **Act on verified state.** Read source files, run real commands before hypothesis or edit. Never guess flag names, library signatures, repo configuration from memory.

2. **Reuse before writing (HISS-19).** Search for the capability first. Extend or call the existing implementation; a second implementation or configuration format is a defect, not redundancy.

3. **Lead with output.** Direct answers, diffs, commands. No filler preamble, no "Based on", no restatement, no chatter.

4. **Context transpiler first.** Never edit `CLAUDE.md`, `.cursor/rules/*.mdc`, `.windsurfrules`, `.github/copilot-instructions.md` manually. All agent instruction updates -> `AGENTS.md`, then:

   ```bash
   praetorctl compile-context
   ```

   - `AGENTS.md` = agent-only text -> caveman (internal register). `praetorctl compile-context --verify` + `praetorctl audit` run caveman lint; findings fail gate; no opt-out. Check first:`praetorctl caveman check AGENTS.md`.

5. **SARIF diagnostic distillation.** Compiler/linter errors -> distill to $\le 1,500$ tokens ($< 60$ lines): top 3 root-cause failures with file/line pointers; full SARIF logs -> ephemeral storage.

6. **No evasion.** Never attempt `--no-verify`, `LEFTHOOK=0`, or modifying `.git/hooks`. `cordana-standards[bot]` re-checks every pull request in ephemeral isolated sandbox.

7. **Anti-loop interception.** Same AST diff + error category repeats $\ge 3$ times -> halt immediately. Re-evaluate design; no micro-textual retries.

8. **State ledger discipline (HISS-17).** Turn start: `praetorctl state status` plus `.workingdir/OPEN.md`, never the whole state ledger. Turn end: `praetorctl state sync .`. `.workingdir` stays private and Git-ignored.

9. **Diff-aware CI (HISS-18).** Use `praetorctl ci filter` for routing. A skipped gate must print the reason and alternate coverage; a gate that did not run is never reported as passing.

## Text Register

<!-- praetor:register:start -->
Register follows the audience, then the task label of your brief (`register:` in `.standards.yaml`; labels are the router's `target_tasks`).

| Register | Where | Form |
| :--- | :--- | :--- |
| social | forge: issues, PR bodies, review comments, commit bodies | `social-text` skill: BLUF, full sentences, scannable, enough and no more; conventional commit subject unchanged; changelog fragment unchanged |
| docs | docs/, README, ADR bodies | complete without bloat: newcomer path first, expert reference after; every claim points at a file, command or test; no restated code |
| internal | briefs, agent-to-agent traffic, research fan-outs, workflow returns | `caveman` skill: fragments, no filler, verbatim code/paths/errors; facts, paths, commands, verdict |

- Task rows: social = commit_message_synthesis, waiver_signoff; docs = architecture_synthesis, function_docstrings; every other label and any unlabeled text = internal. Subagent launch brief: `caveman` brief shape with `task:` = routing label.
- Evidence above 58 lines or 1500 tokens leaves the message as a file under `.workingdir/evidence/`; return `evidence: <path> sha256:<12 hex> lines:<n>` and fetch it only when a decision needs it.
- An internal return carries verdict, changed paths, commands run, evidence pointers and open questions, nothing else.
<!-- praetor:register:end -->

## Primary Verification Commands

```bash
# Fast local test suite
# Declared commands only; run them before claiming application verification.
'make' 'verify-all'

# Recompile and verify cross-agent context outputs
praetorctl compile-context --verify

# Audit repository against declared HISS-21 standards
praetorctl audit

# Replay every declared scanner claim against its fixture corpus
praetorctl hiss coverage --verify

# Run all formatting, linting, and security gates
make verify-all
```

<!-- praetor:harness:end -->

---

<!-- markdownlint-disable MD013 -->
# AGENTS.md — VMAFx

> **CORRECT REPO: `VMAFx/vmafx` (active) — NOT `lusoris/vmaf` (archived)**
> Fork renamed + transferred. All work, PRs, gh commands -> `VMAFx/vmafx`. Session start: `gh repo set-default VMAFx/vmafx`.

## GLOBAL PROJECT RULES (TOP PRIORITY)

1. NEVER modify Netflix golden-data assertions (`python/test/` `assertAlmostEqual`). Exception: verbatim upstream port ([ADR-1828](docs/adr/1828-port-netflix-golden-updates.md)).
2. EVERY user-discoverable surface gets human-readable documentation in same PR.

## 1. What this repo is

- Fork of [Netflix/vmaf](https://github.com/Netflix/vmaf): perceptual video quality assessment.
- Additions: GPU backends (SYCL/CUDA/HIP runtime-selected), AVX2/AVX-512/NEON SIMD, tiny-AI (ONNX Runtime; `ai/`, `core/src/dnn/`), MCP server (`mcp-server/vmaf-mcp/`).
- CLI `--precision`: default `%.6f` (Netflix-compat), `--precision=max` opts in to `%.17g` IEEE-754 round-trip lossless ([ADR-0119](docs/adr/0119-cli-precision-default-revert.md)).
- Licence: per-file `SPDX-License-Identifier` authoritative; fork EUPL-1.2 ([ADR-1250](docs/adr/1250-eupl-fork-relicense.md)), inherited BSD-2-Clause-Patent; default branch `master`, remote `upstream`.

## 2. Build, test, lint

```text
meson setup build core [-Denable_cuda=true|false] [-Denable_sycl=true|false]
ninja -C build
python3 scripts/ci/run_meson_test.py -- -C build [--suite=fast]
make test
make test-netflix-golden
make preflight     # CI compiler matrix locally before push
make lint
make format        # writes
make format-check  # dry-run
```

- Build guide: [docs/getting-started/index.md#build-from-source-any-platform](docs/getting-started/index.md#build-from-source-any-platform)
- Backend guides: [docs/backends/index.md](docs/backends/index.md)

## 3. Coding standards

- Read [docs/principles.md](docs/principles.md) before writing C. Conforms to NASA/JPL Power of 10, JPL C, CERT C/C++, MISRA subset; K&R 4-space, 100 cols.
- Banned: `gets`, `strcpy`, `strcat`, `sprintf`, `strtok`, `atoi`, `atof`, `rand`, `system`.
- Every non-void return checked, handled; casting to `(void)` does not handle error.

## 4. Netflix golden gate

- Three reference CPU pairs = numerical ground truth. Never modify assertions in `python/test/` except verbatim upstream ports.
- Isolated build profile runs via `make test-netflix-golden`; full detail: [docs/development/agent-reference.md#netflix-golden-data-gate](docs/development/agent-reference.md#netflix-golden-data-gate).

## 5. Snapshots

- `testdata/scores_cpu_*.json`, `testdata/netflix_benchmark_results.json` = fork snapshots (not golden data).
- Intentional numeric change -> regenerate via `/regen-snapshots` with commit justification.

## 6. Upstream sync

- Remote setup: `git remote add --no-tags upstream https://github.com/Netflix/vmaf.git`; sync via skills `sync-upstream` or `port-upstream-commit`.

## 7. Release

- `release-please` triggers on pushes to `master`. 0.x line restarted at `0.1.0` ([ADR-3292](docs/adr/3292-release-line-restart-0x.md)); pre-1.0 breaking = minor bump; release candidates only for 1.0.0.
- Milestone map: [docs/development/release.md#release-map](docs/development/release.md#release-map).
- Stage definitions and gate rules: [docs/development/agent-reference.md#release-stages](docs/development/agent-reference.md#release-stages).

## 8. Hard rules and invariants

- Binding hard rules: read before first commit. Invariants page: read relevant section before rebase, upstream sync, conflict resolution, or editing cited files.
- [Agent hard rules](docs/development/agent-hard-rules.md) — golden gate, branch/commit rules, same-PR docs, license headers, state tracking.
- [Worktree discipline](docs/development/agent-worktree-discipline.md) — isolated agent worktrees, host guard.
- [Rebase-sensitive invariants](docs/development/rebase-sensitive-invariants.md) — index preserved across upstream sync and rebase.

## 9. Reference documentation

- Full reference material: [docs/development/agent-reference.md](docs/development/agent-reference.md) (repository layout, skills table, golden detail, release stages).

## 10. Interaction style

- Prefer structured UI questions (AskUserQuestion etc.) with 2-4 options, `(Recommended)` marked when obvious; answers and reporting in prose fine.
