# ADR-0951: GitHub Actions custom-action and reusable-workflow audit

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `ci`, `docs`, `process`

## Context

The fork carries 24 workflow files under `.github/workflows/` but no
in-tree custom composite/JS actions (`.github/actions/` does not exist)
and no reusable workflows (`workflow_call:` does not appear in any
file). All external `uses:` directives are already SHA-pinned per the
existing invariant in [`.github/AGENTS.md`](../../.github/AGENTS.md)
(SHA-pin block at lines 100–144, enforced by the sync gate +
`scorecard.yml`).

The audit asked for the standard four checks against any in-tree
custom action (SHA-pinned external refs, declared input/output schema,
description present, error-handling for missing inputs) plus a search
for reusable-workflow opportunities across the 24-file matrix.

## Decision

We accept the current state as audited and **do not add a custom
action or reusable workflow in this PR**. Two abstraction candidates
are documented below for future opportunistic refactors when the next
workflow file is added or substantially rewritten; we will not
preemptively migrate the 8–10 existing call sites.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Add `.github/actions/setup-build-deps/action.yml` (composite) and migrate the 8 `apt-get install meson ninja-build nasm pkg-config …` call sites now | Removes the most-duplicated boilerplate in the tree; one place to bump pin versions | Touches 8 workflow files in one PR (>200 LOC churn); risks cascading required-check renames; the composite would still need per-callsite overrides for extra packages (`clang-tidy-18`, `intel-oneapi-compiler-dpcpp-cpp`, `libcudart12`) which negates much of the win | Deferred — opportunistic migration when each call site is next edited is cheaper and lower-risk than a big-bang sweep |
| Add a reusable `meson-cpu-build.yml` (`workflow_call:`) and let `tests-and-quality-gates.yml` / `sanitizers.yml` / `lint-and-format.yml` invoke it for the CPU-only build phase | Higher payoff than composite — eliminates the entire `meson setup build -Denable_cuda=false -Denable_sycl=false …` block, not just the apt line | Reusable workflows run as a separate job (extra runner spin-up, cache-key churn) and cannot share the parent job's `steps` ordering; the build flags vary subtly across call sites (`--buildtype=release` vs `--buildtype=debug`, AVX-512 on/off) | Deferred — the per-call-site variation in build flags would need 5–6 inputs, eroding the abstraction's value |
| No-op (current state, document audit findings only) | Zero risk; the workflows are already SHA-pinned and conform to the security policy in ADR-0263 | Leaves the ~8-site apt-install duplication unfixed | **Chosen** — the audit's primary deliverable is "verify no gaps in custom actions or reusable workflows"; both abstractions are listed in the digest for future PRs |

## Consequences

- **Positive**: documents the audit outcome (no custom actions exist;
  all externals SHA-pinned) so the next sync agent does not redo the
  search. The two deferred abstraction candidates are captured in the
  digest under [`docs/research/`](../research/github-actions-custom-audit-2026-05-31.md)
  for opportunistic pickup. See
  [`docs/research/0951-github-actions-custom-audit.md`](../research/0951-github-actions-custom-audit.md).
- **Negative**: the duplicated apt-install lines remain in 8 call
  sites until a future PR migrates them. Each new workflow file added
  before that migration also pays the duplication cost.
- **Neutral / follow-ups**: when the next workflow file edit touches
  one of the duplicated patterns, the editor should opt to extract the
  composite action as a side effect rather than copy the pattern
  again.

## References

- [`.github/AGENTS.md`](../../.github/AGENTS.md) — SHA-pin invariant
  (lines 100–144).
- [ADR-0263](0263-ossf-scorecard-policy.md) — OSSF Scorecard pin
  policy.
- Research digest: [`docs/research/0951-github-actions-custom-audit.md`](../research/0951-github-actions-custom-audit.md).
- Source: `req` — "Audit `.github/actions/` (custom composite/JS
  actions if any) + workflow reusable patterns."
