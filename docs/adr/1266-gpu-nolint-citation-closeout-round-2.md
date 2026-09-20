# ADR-1266: Exact-comment NOLINT citation scan and tree-wide closeout

- **Status**: Accepted
- **Date**: 2026-09-20
- **Deciders**: Lusoris
- **Tags**: lint, cleanup, touched-file-rule, sycl, cuda, hip, ci

## Context

[ADR-0141](0141-touched-file-cleanup-rule.md) requires every `NOLINT` marker to
cite, inline, the ADR, research digest, or rebase invariant that makes the
suppression necessary. `scripts/ci/tidy-ratchet.py::count_uncited_nolints` is
the ratchet's enforcement for that rule.

The shipped counter did not have a coherent comment boundary. It recognised
`/* ... */` only in one direction, did not recognise a contiguous explanatory
run of `//` lines, and also accepted an `ADR-NNNN` token on the previous or next
physical line even when that token belonged to another comment or to code. Two
errors therefore partially cancelled on `origin/master` at `8d0cdd7c4`:

| measurement                                          | markers | paths |
| ---------------------------------------------------- | ------: | ----: |
| reported by the shipped scanner                      |      46 |    25 |
| false reports already cited inside their own comment |      16 |    16 |
| real violations hidden by the physical-line shortcut |      33 |    23 |
| exact lexical scan (`46 - 16 + 33`)                  |      63 |    32 |

The 16 false reports comprise two block comments with the citation before the
marker, two CUDA `//` runs with the citation before it, and twelve SYCL `//`
runs with the citation after it. The 33 hidden violations borrowed a citation
from outside the marker's own comment.

The shipped scanner's 30 genuinely uncited markers also needed a refactor-first
review. Their original rationales were not proof that the suppressions were
necessary:

- seven C-linkage brackets claimed TU-local callbacks could not live in an
  anonymous namespace, although only the exported extractor descriptor needs C
  linkage;
- twenty kernel-site markers covered a mixture of simple tidy fixes, dead
  suppressions, and template-specialisation diagnostics; and
- three parity-test function-size suppressions covered code that could be split
  into cleanup-safe helpers.

## Decision

Define one strict lexical contract: a marker is cited only when `ADR-NNNN`
occurs in the exact comment containing it. A comment is either one complete
`/* ... */` block or one maximal contiguous run of `//` lines separated only by
a newline and optional horizontal whitespace. Adjacent independent comments,
code, and string, character, or raw-string literals cannot lend a citation.
Multiple markers count individually; `NOLINTEND` does not count.

Implement that contract with a small comment lexer in `count_uncited_nolints`,
remove the previous/same/next physical-line shortcut, and pin the boundary cases
with focused tests.

Resolve the 63 violations as follows:

| population                                 | resolution                                                                                                                                                           |
| ------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 7 linkage brackets from the original 30    | move TU-local implementations into anonymous namespaces; retain explicit C linkage only on exported descriptors                                                      |
| 8 kernel markers from the original 30      | apply semantics-neutral code fixes (`const` pointer bindings, split declarations, branch simplification, unused-parameter removal, and non-mutating subgroup lookup) |
| 10 kernel markers from the original 30     | delete diagnostics-free suppressions after LLVM 20, 21, and 22 probes                                                                                                |
| 3 parity-test markers from the original 30 | extract cleanup-safe helpers and delete the suppressions                                                                                                             |
| 2 template markers from the original 30    | retain exact citations: scale 3 folds `DO_RD` writes away, while scales 0-2 instantiate and mutate the same accumulators                                             |
| 33 violations hidden by adjacency          | delete one dead HIP marker and move 32 citations into the exact marker comment                                                                                       |

The exact tree-wide uncited count is now zero. A broader audit also removed four
already-cited but diagnostics-free code suppressions outside those 63 and
refactored two duplicate function-size suppression pairs in the SYCL
`motion_add_uv` parity test.

Changing scanner semantics changes every lane's measurement. All four tidy
ratchet lanes must therefore be remeasured in full; scoped baseline writes are
not sufficient for this change.

## Alternatives considered

| Option                                                   | Pros                                                      | Cons                                                                                                                       | Why not chosen                                                                              |
| -------------------------------------------------------- | --------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| Keep the previous/same/next-line shortcut                | Minimal scanner change                                    | An unrelated neighbouring comment or code string can satisfy the rule; master appears to have 46 violations when it has 63 | It makes the citation non-local and hides real debt                                         |
| Correct comment parsing, then cite every reported marker | Small source edits                                        | Preserves false ABI, kernel, and test rationales; violates ADR-0141's refactor-first rule                                  | A citation is not evidence that a suppression is necessary                                  |
| Exact scan plus refactor-first review                    | Makes the count and each surviving suppression meaningful | Touches more files and requires full lane remeasurement                                                                    | Chosen: it enforces the actual policy rather than its historical accidents                  |
| Remove the two surviving template suppressions too       | Zero retained markers in the audited kernel sites         | Requires splitting template control flow or adding meaningless writes solely to appease one folded specialization          | The variables are genuinely mutable in scales 0-2, so the two suppressions are load-bearing |

## Consequences

- **Positive**: the counter now measures exact explanatory ownership. Its unit
  suite has 27 passing tests, including 18 scanner-specific cases covering both
  comment syntaxes, boundaries, literals, multiple markers, and `NOLINTEND`.
- **Positive**: 28 of the original 30 suppressions were removed rather than
  decorated; only two site-specific template suppressions survive.
- **Positive**: the seven exported SYCL extractor descriptors retain C linkage
  while their implementations now have ordinary C++ internal linkage.
- **Negative**: the stricter scanner raises the historical master measurement
  from 46 to 63, so all lane baselines must be regenerated even though the
  corrected working tree ends at zero.
- **Neutral / follow-up**: the ratchet baseline's SYCL measured-source list
  still omits `core/src/feature/sycl/*.cpp`. The separate changed-file SYCL
  wrapper can analyse them, but the two surviving markers are not represented in
  the lane baseline. [`docs/state.md`](../state.md) tracks that gap.

## Verification

- Historical blob measurement: shipped `46 / 25 paths`; exact `63 / 32`.
- Current exact tree scan: `0 / 0 paths`.
- `python3 -m pytest scripts/ci/tests/test_tidy_ratchet.py -q`: 27 passed.
- Full SYCL and HIP builds succeeded; focused registration/parity suites passed.
  This host has no usable SYCL device, and the HIP float-VIF path reaches its
  documented `-ENOSYS` scaffold skip, so those runs do not claim device-level
  numeric parity.

## References

- [ADR-0141](0141-touched-file-cleanup-rule.md) - inline-citation and
  refactor-first rule.
- [ADR-0278](0278-t7-5-nolint-sweep.md) - first NOLINT closeout.
- [ADR-1142](1142-whole-codebase-standards.md) - whole-tree standards scope.
- [ADR-1243](1243-tidy-scoped-baseline-tightening.md) - why scoped writes cannot
  replace full remeasurement here.
- Research digest:
  [`docs/research/2067-nolint-citation-scan-audit.md`](../research/2067-nolint-citation-scan-audit.md).
- Source: `req` - the user directed that every open defect be fixed rather than
  deferred ("pre-existing is no excuse ever"; "fix all").
