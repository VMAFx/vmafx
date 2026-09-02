<!-- markdownlint-disable MD013 MD060 -->
# ADR-0167: Path-mapped doc-drift enforcement (local hook + CI gate)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: process, enforcement, claude-hook, ci, adr-0100

## Context

[ADR-0100](0100-project-wide-doc-substance-rule.md) and CLAUDE.md §12
rule 10 mandate that every PR changing a user-discoverable surface
ships human-readable documentation under `docs/` in the same PR. The
ADR's intent is clear: ADRs explain decisions to maintainers; reference
docs explain usage to users; the two are not interchangeable.

The 2026-04-25 docs audit (Claude Explore agent, scope: 16 PRs landed
2026-04-22 → 2026-04-25) surfaced concrete drift:

- `vmaf_cuda_state_free()` API (PR #94, ADR-0157) — declared in the
  public CUDA header, **completely undocumented** in
  [`docs/api/gpu.md`](../api/gpu.md).
- `-EAGAIN` return code (PR #91, ADR-0154) — added to the public API,
  **missing from the error-codes table** in
  [`docs/api/index.md`](../api/index.md).
- SSIMULACRA 2 SIMD ports (PRs #98 / #99 / #100, ADR-0161/0162/0163) —
  three full SIMD ports landed across AVX2 / AVX-512 / NEON;
  [`docs/metrics/features.md`](../metrics/features.md) still claimed
  the metric was "scalar only. SIMD / GPU paths are follow-up
  workstreams" 36 hours after the last SIMD PR merged.
- `psnr_hvs` AVX2 + NEON (PRs #96 / #97) — same pattern.
- `float_ms_ssim` <176×176 rejection (PR #90, ADR-0153) — new
  `-EINVAL` semantics not documented.
- `vmaf_read_pictures` monotonic-index requirement (PR #88,
  ADR-0152) — same.

Every one of these PRs **passed** the existing
`doc-substance-check` advisory job. Two reasons why:

1. **Advisory only.** The job ran with `continue-on-error: true` —
   it could post a comment but never fail the check.
2. **Coarse-grained.** It hit if **any** `docs/` file changed,
   including a newly-added ADR. PRs that landed a thorough ADR
   under `docs/adr/` "satisfied" the check even when no
   user-facing reference doc was touched.

The user direction on 2026-04-25 closed the gap:

> "I bet we as well should check if the docs are fully in state with
> codebase vs rules etc... seems like this has gaps somehow"
>
> _(later, after seeing the audit findings):_ "do this now as well"

This ADR locks the tightening as a permanent rule.

## Decision

Add **two layers** of doc-drift enforcement:

### Layer 1 — Project hook (informational, in-session)

New PostToolUse hook
[`.claude/hooks/docs-drift-warn.sh`](../../.claude/hooks/docs-drift-warn.sh)
fires on every Edit/Write that touches a user-discoverable surface
file. It maps the surface path to its expected `docs/<topic>/` path
and emits a `NOTICE:` to stderr if the expected docs file:

- has not been edited in the working tree (no `git status` change), AND
- has not been touched more recently than the surface file.

It **does not block** — same convention as the existing
`auto-snapshot-warn.sh` hook. The goal is to remind the agent
in-session, before the PR is opened, while context is still fresh
and the docs update is cheap.

### Layer 2 — CI gate (blocking, pre-merge)

[`.github/workflows/rule-enforcement.yml`](../../.github/workflows/rule-enforcement.yml)
job `doc-substance-check` is promoted from advisory to **blocking**
and rewritten to use a **path-mapped check**:

- A surface-regex maps to a docs-regex.
- The job fails if the PR touches a surface file but no path-mapped
  docs file gets edited in the same PR.
- ADR additions under `docs/adr/` are **explicitly excluded** from
  satisfying the docs-hit — the entire point of the audit was that
  ADRs are decisions, not usage.
- A per-PR opt-out `no docs needed: REASON` in the PR body
  satisfies the check for genuine internal-refactor / bug-fix / test
  PRs with no user-visible delta.

The mapping covers:

| Surface | Expected docs path |
|---|---|
| `core/include/libvmaf_cuda.h`, `libvmaf_sycl.h` | `docs/api/gpu.md` |
| `core/include/libvmaf_dnn.h` | `docs/api/dnn.md` |
| `core/include/{libvmaf,picture,model}.h` | `docs/api/index.md` |
| `core/src/feature/{feature_,integer_}*.c` | `docs/metrics/` |
| `core/src/feature/{x86,arm64}/*.c` | `docs/metrics/` |
| `core/src/feature/cuda/*.{c,cu}` | `docs/metrics/` or `docs/backends/cuda/` |
| `core/src/feature/sycl/*.cpp` | `docs/metrics/` or `docs/backends/sycl/` |
| `core/tools/{cli_parse,vmaf,vmaf_bench}.c` | `docs/usage/` |
| `core/meson_options.txt` | `docs/development/build-flags.md` |
| `mcp-server/vmaf-mcp/*` | `docs/mcp/` |
| `ai/src/vmaf_train/cli/*` | `docs/ai/` |
| `ffmpeg-patches/*.patch` | `docs/usage/` |

The mapping is intentionally minimal — a maintainer can extend it
in a follow-up ADR as new surfaces appear (e.g. new GPU backend,
new tiny-AI submodule).

## Alternatives considered

1. **Tighten only the CI gate, skip the local hook.** Rejected: the
   moment-of-edit reminder is the cheapest place to catch drift,
   and the CI round-trip costs minutes per cycle. Doing both is
   defence in depth.
2. **Tighten only the local hook, leave CI advisory.** Rejected: the
   hook only fires inside Claude Code sessions. PRs from other
   contributors / agents / direct-IDE edits would slip through.
   The CI gate is the irreducible authority.
3. **Block on ADR additions equally to docs/ additions.** Rejected
   for the opposite reason — ADRs are still required for non-trivial
   decisions per CLAUDE.md §12 rule 8, and conflating them with
   user-facing docs would either over-trigger this check or
   weaken the ADR rule.
4. **Make the path map declarative** (e.g. a YAML map at
   `.github/doc-coverage-map.yml`). Tempting, but adds a new file
   and parsing surface for what is currently a single-digit number
   of mappings. Defer until the map exceeds ~20 rows.
5. **Per-feature docs (one file per metric)** instead of a single
   `docs/metrics/features.md`. Out of scope for this ADR; the
   path-mapped check works either way.

## Consequences

**Positive:**

- The drift class that produced this audit becomes statically
  unmergeable: a SIMD port for `ssimulacra2` cannot land without
  touching `docs/metrics/features.md` (or claiming
  `no docs needed:`).
- Local hook gives sub-second feedback inside Claude Code sessions.
- ADR-bearing PRs no longer accidentally satisfy the check by
  virtue of adding the ADR — documentation hygiene is decoupled
  from decision logging.

**Negative:**

- Path map maintenance: every new user-discoverable surface needs a
  new mapping row. Acceptable cost — adding a backend / metric /
  CLI subcommand is rare.
- False positives possible (e.g. an internal refactor of
  `feature_psnr.c` legitimately requires no docs update). The
  per-PR opt-out `no docs needed: REASON` handles this; reviewers
  verify the reason.
- Local hook adds ~50 ms per Edit/Write call. Negligible compared
  to the formatter pass already in place.

## Rollout

This ADR + the hook + the workflow tighten land in one PR
(`feat/t7-state-mcp-release-runner`, 2026-04-25). The PR itself
satisfies the new check by virtue of touching `docs/api/gpu.md`,
`docs/api/index.md`, `docs/metrics/features.md`,
`docs/development/`, `docs/mcp/` precursors, and `docs/state.md`.

Pre-existing PRs do not retroactively trigger.

## References

- [ADR-0100](0100-project-wide-doc-substance-rule.md) — the parent
  rule this enforces.
- [ADR-0124](0124-rule-enforcement-ci.md) — the original
  rule-enforcement workflow scaffolding.
- [`.claude/hooks/auto-snapshot-warn.sh`](../../.claude/hooks/auto-snapshot-warn.sh) —
  pattern this hook copies (informational stderr, no block).
- 2026-04-25 docs audit (Claude Explore agent transcript, full
  findings in PR description).
- `req` — user direction 2026-04-25: "do this now as well" (after
  reviewing the audit findings + diagnosis of why the existing
  workflow missed them).
