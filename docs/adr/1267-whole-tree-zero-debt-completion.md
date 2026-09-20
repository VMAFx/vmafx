# ADR-1267: Whole-tree warning and standards debt must reach zero

- **Status**: Accepted
- **Date**: 2026-09-20
- **Deciders**: Lusoris
- **Tags**: ci, lint, cleanup, code-quality, agents, cuda, sycl, hip, simd

## Context

The canonical [`AGENTS.md`](../../AGENTS.md) already declares HISS-10 as
"zero-warning tolerance across compiler, linter, and format sweeps." Older
policy still described a different endpoint: [ADR-0141](0141-touched-file-cleanup-rule.md)
required only edited files to reach zero, while [ADR-1142](1142-whole-codebase-standards.md)
allowed untouched findings to remain indefinitely behind per-file baselines.
That contradiction caused a live closeout to treat 1,378 HISS findings,
4,288 lane-counted clang-tidy diagnostics, and a hidden whole-tree assertion
density queue as accepted legacy debt.

Origin never changes the engineering standard. Netflix-mirror, vendored,
generated-from-source, fork-local, GPU, SIMD, tests, and tools are all product
code when they are built or shipped. A baseline can locate debt during a
migration; it cannot make a warning or error compliant.

## Decision

The repository will eliminate every actionable warning and standards finding
across the full tree and then enforce zero as the steady-state gate.

1. **Zero is the only completion state.** Pinned Praetor HISS, clang-tidy in
   every configured lane, cppcheck, compiler warnings, language linters,
   formatters, and repository invariant checks must report zero actionable
   findings and zero tool or parse failures. A skipped or unavailable lane is
   reported as not run, never as clean.
2. **No origin carve-out exists.** Netflix-mirror and vendored code is fixed in
   place with its rebase or re-vendor invariant recorded. Generated output is
   fixed at its generator. Tests, tools, GPU kernels, SIMD, and compatibility
   code receive the same treatment as fork-local production code.
3. **Baselines are migration inventories only.** Reviewable cleanup waves may
   use the existing JSON inventories to partition and prove monotonic progress,
   but work continues without declaring repository completion until every
   inventory is empty. No warning is accepted merely because it is baselined
   or outside a pull request's changed-file set.
4. **Refactor before suppression.** A `NOLINT` or analyzer suppression remains
   permissible only when removing it would violate a stronger, demonstrated
   invariant such as numerical bit-exactness or a public ABI. It must be
   site-specific, cite the governing evidence in its own comment, and still be
   included in the suppression audit. Source origin is never a rationale.
5. **Numerical truth remains above cleanup.** Netflix golden assertions are
   never changed. Arithmetic-sensitive rewrites require scalar/SIMD or
   cross-backend evidence appropriate to the affected path.
6. **Gates become zero gates.** When a lane reaches zero, its baseline is
   recorded as an empty inventory and may not rise. After the final wave, the
   ratchet compatibility path is removed or reduced to asserting exact zero;
   changed-file-only and advisory jobs do not substitute for the whole-tree
   result.

This supersedes ADR-0141's touched-file acceptance boundary and ADR-1142's
ratchet-as-end-state decision. Their historical measurements and sequencing
rationale remain useful, but not their permission to retain debt.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep the per-file ratchet indefinitely | Prevents regression with manageable PRs | Treats recorded warnings as a permanent compliant state; contradicts HISS-10 | Rejected: a bound is not zero-warning compliance |
| Enforce only changed files at zero | Fast feedback and small diffs | Leaves untouched, upstream, vendored, and backend code outside the completion boundary | Rejected: this is the ambiguity the maintainer explicitly ended |
| One monolithic cleanup PR | Reaches the final number in one merge | Hundreds of files, weak reviewability, high numerical-regression risk | Rejected: the campaign may land tested waves, but it may not stop between them |
| Reviewable waves with a mandatory zero endpoint | Preserves focused test evidence while continuously reducing the global queues | Requires sustained work and repeated full-tree measurements | Chosen |

## Consequences

- **Positive**: green means no known standards debt rather than "no debt above
  the allowance"; code origin can no longer hide a finding.
- **Positive**: each cleanup wave carries an exact before/after receipt and the
  final baselines are mechanically empty.
- **Negative**: hundreds of functions and control-flow sites require
  arithmetic-preserving refactors plus broad platform testing.
- **Negative**: removing the assertion-density origin and storage-class blind
  spots exposed 1,473 non-trivial functions across 423 files with no runtime
  invariant assertion; those are part of the mandatory cleanup, not a reason
  to restore the blind spots.
- **Negative**: all configured GPU/toolchain lanes must be remeasured; a lane
  without an executable toolchain blocks a whole-tree-clean claim.
- **Neutral / follow-ups**: update `agent-hard-rules.md`, `principles.md`, CI
  documentation, workflow comments, and PR templates that still describe a
  touched-file or permanent-ratchet endpoint. Retire true source exclusions as
  their toolchain profiles are made executable.

## References

- Source (`req`, verbatim, 2026-09-20): "there is no on touch rule anymore, no
  fucking warning or error is just ignored because of being og netflix code,
  fix them all ffs, how often do i have to repeat thi".
- [`AGENTS.md`](../../AGENTS.md), HISS-10: zero-warning tolerance across
  compiler, linter, and format sweeps.
- [Research-2068](../research/2068-whole-tree-zero-debt-inventory.md): initial
  exact inventory, scope, and verification contract.
- Supersedes the acceptance boundaries in
  [ADR-0141](0141-touched-file-cleanup-rule.md) and
  [ADR-1142](1142-whole-codebase-standards.md).
