# ADR-0885: Rename fork-added header guards away from reserved-identifier form

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude Code agent
- **Tags**: cleanup, lint, principles, cert-c

## Context

C99 §7.1.3 reserves all identifiers that begin with an underscore followed
by an uppercase letter, or that contain two consecutive underscores, for
the implementation. The widespread `__FOO_H__` header-guard pattern
violates both criteria simultaneously. `clang-tidy`'s
`bugprone-reserved-identifier` and the SEI CERT C rule `DCL37-C` both flag
it.

PR #327 (T7-5 NOLINT sweep, [ADR-0278](0278-t7-5-nolint-sweep.md)) closed
out reserved-identifier suppressions for three public headers
(`feature.h`, `model.h`, `dnn.h`) that PR retained as upstream-mirror or
ABI-frozen surfaces, with inline ADR cites in lieu of renames. The audit
that produced that sweep did not extend to the rest of the fork-added
header tree; eleven additional fork-added headers were still using
`__VMAF_X_H__` guards. The fork's "touch-it, lint-clean-it" rule
([ADR-0141](0141-touched-file-cleanup-rule.md)) requires those to be
either renamed or cited the next time anyone touches them, so they are
discharged now as a one-shot sweep rather than incremental drift.

## Decision

We rename the include-guard macros of all eleven fork-added headers from
the reserved form `__VMAF_<X>_H__` to the conformant form
`VMAF_<X>_H_`. Leading-underscore prefix is dropped; trailing
double-underscore is replaced with a single trailing underscore (still
non-reserved). Upstream-mirror headers (`libvmaf.h`, `feature.h`,
`model.h`, `picture.h`, `macros.h` from the upstream tree, internal
`cpu.h`, `dict.h`, etc.) and third-party headers (cJSON) are left
untouched to preserve rebase compatibility. The three public headers
already cited by [ADR-0278](0278-t7-5-nolint-sweep.md) are also left
untouched.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rename all eleven fork-added headers (chosen) | Eliminates one class of CERT-C / clang-tidy violations across the whole fork tree in one pass; future touches no longer trigger the rule. | One sweep PR; reviewers must walk eleven small renames. | Cheapest path; identifiers are local to each header (3 sites each), no ABI / API impact. |
| Suppress with `// NOLINT` cite per header | Preserves the symmetric `__VMAF_X__` style across mirror and fork-added files. | Doesn't actually fix the conformance issue; perpetuates a suppression we have a fix for; conflicts with [ADR-0141](0141-touched-file-cleanup-rule.md) which prefers refactor over suppression. | NOLINT is reserved for load-bearing invariants; aesthetic symmetry with upstream isn't one. |
| Also rename the upstream-mirror headers | Removes the divergence from CERT-C across the whole tree. | Would diverge from Netflix/vmaf master; every upstream sync would re-introduce the original names; would also have to chase identical renames in any upstream-mirror `.c` files that reference the guards. | Rebase cost outweighs lint cleanliness; ADR-0278's three-header cite continues to handle the public-API case. |

## Consequences

- **Positive**: Eleven fork-added headers become CERT-C DCL37-C and
  `bugprone-reserved-identifier` clean; future touches to those headers
  no longer trip those rules during `make lint`.
- **Negative**: One-time sweep diff; the guards no longer visually match
  the upstream-mirror neighbours (`__VMAF_FOO_H__` next to
  `VMAF_BAR_H_`).
- **Neutral / follow-ups**: Any new fork-added header MUST use the
  non-reserved form from the start. Upstream-sync pulls that introduce
  new `__VMAF_X__` guards in headers we mirror unchanged are still
  permitted (rebase compatibility wins).

## References

- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-cleanup rule.
- [ADR-0278](0278-t7-5-nolint-sweep.md) — NOLINT citation closeout that
  covered the three public-header exceptions.
- `req`: lusoris dispatch 2026-05-30 — "Audit C/C++ header include-guards
  for reserved-identifier violations (CERT DCL37-C / `__SOMETHING__`
  patterns)."
- SEI CERT C — DCL37-C: Do not declare or define a reserved identifier.
- ISO/IEC 9899:1999 §7.1.3 Reserved identifiers.
