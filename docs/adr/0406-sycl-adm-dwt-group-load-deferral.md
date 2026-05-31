<!-- markdownlint-disable MD013 MD060 -->
# ADR-0406: Defer SYCL ADM DWT `group_load` rewrite — divisibility blocker

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: lusoris, Claude (Opus 4.7)
- **Tags**: sycl, adm, perf, deferred, fork-local

## Context

[Research-0086 §A.4](../research/0086-sycl-toolchain-audit-2026-05-08.md)
emitted a GO recommendation to rewrite the ADM DWT vertical and
horizontal passes in
[`integer_adm_sycl.cpp`](../../core/src/feature/sycl/integer_adm_sycl.cpp)
on top of `sycl::ext::oneapi::experimental::group_load` /
`group_store`, citing claimed register-pressure reduction on Intel
Xe2 (Battlemage). This ADR records the implementation-time finding
that the sketched rewrite does not survive the math and the kernel
shape constraints, and the decision to defer.

Two technical blockers were identified during implementation:

1. **Divisibility**: the vertical-pass cooperative tile is
   `TILE_H × WG_X = 18 × 32 = 576` int32 elements loaded by
   `WG_SIZE = WG_X × WG_Y = 32 × 8 = 256` work-items. The SYCL
   extension contract for `group_load` (header
   `/opt/intel/oneapi/compiler/2025.3/include/sycl/ext/oneapi/experimental/group_load_store.hpp`,
   functions at lines 447, 462, 476, 490, 505, 519) requires
   `total = WG_SIZE × ElementsPerWorkItem` with every work-item in
   the group loading the same `ElementsPerWorkItem` count.
   `576 / 256 = 2.25`, not an integer — no choice of
   `ElementsPerWorkItem` covers the tile. The general expression
   `TILE_ELEMS / WG_SIZE = 2 × WG_X × (WG_Y + 1) / (WG_X × WG_Y) =
   2 × (WG_Y + 1) / WG_Y` is integer only for `WG_Y ∈ {1, 2}` —
   neither viable for an 8-row output stride.
2. **Source contiguity**: a tile row is contiguous in input memory
   (`WG_X = 32` ints starting at
   `(row_start + tr) × in_stride + tile_col`) but successive tile
   rows are separated by full `in_stride`. `group_load` takes an
   `InputIteratorT` over a contiguous range
   ([line 281](https://github.com/intel/llvm/blob/sycl/sycl/include/sycl/ext/oneapi/experimental/group_load_store.hpp));
   it cannot consume the strided multi-row tile in one call.
   Per-row invocation would require `ElementsPerWorkItem = 32 / 256
   < 1` — also not legal.

The horizontal pass at
[`integer_adm_sycl.cpp:358`](../../core/src/feature/sycl/integer_adm_sycl.cpp)
was a non-target from the start. It carries no SLM tile; each
work-item reads four mirrored input columns directly. There is no
cooperative load to replace with `group_load`.

The Battlemage register-pressure claim that motivated the GO
recommendation is unverifiable on the dev host (Arc A380 Alchemist

- AMD Granite Ridge CPU; no Xe2 / Battlemage GPU available, see
[research-0086 §A.4 author note](../research/0086-sycl-toolchain-audit-2026-05-08.md)).

## Decision

We do not rewrite the ADM DWT vert / hori passes on top of
`group_load` / `group_store` at this time. The kernel keeps its
current manual cooperative tile-load shape. The post-oneAPI-2025.3
audit checklist row in
[`docs/development/oneapi-install.md`](../development/oneapi-install.md)
remains unticked, with this ADR cited as the rationale.

The path is reopened only when both of the following are satisfied:

- a tile-geometry redesign that produces an integer
  `TILE_ELEMS / WG_SIZE` (e.g. `WG_Y = 2` with widened `WG_X`, or a
  fully restructured per-row load loop), validated to produce
  bit-identical outputs against the current SLM path under
  `scripts/ci/cross_backend_vif_diff.py --feature adm --backend sycl`
  at `places=4`; AND
- Battlemage (Xe2) hardware available to the agent to confirm the
  register-pressure delta claimed by the digest.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Full rewrite per digest sketch | Matches digest GO recommendation; collapses 4-arm bpc/scale conditional. | Sketch is mathematically incorrect (576/256 = 2.25 is not integer); would not compile against the SYCL ext contract. | Premise fails review. |
| Hybrid: `group_load` for divisible bulk + manual remainder | Lets us use `group_load` for 16 of the 18 tile rows. | Two load paths in one kernel = more code, not less; defeats the digest's "code-shape simplification" win; requires a second SLM barrier; perf delta cannot be measured (no Battlemage). | Net-negative complexity without the perf evidence to justify it. |
| Restructure to `WG_Y = 2` | Yields integer divisibility (`TILE_ELEMS = 6 × WG_X` divides evenly into `WG_SIZE = 2 × WG_X`). | 4× more work-groups dispatched; loses the per-WG amortisation that motivated `WG_Y = 8` originally; no perf evidence either way. | Out of scope for an A.4 follow-up; would need its own perf-tuning research digest. |
| Hoist the bpc/scale conditional out of the inner loop only (no `group_load`) | Independent simplification; preserves bit-exactness; small diff. | Doesn't deliver the digest's register-pressure claim; the conditional on `e_scale` and `e_bpc` is already loop-invariant and the icpx 2025.3 LLVM-20 codegen hoists it (verified by inspection of the existing kernel — the values come from kernel-launch capture, so they are compile-time within each kernel instantiation only if templated; today they are runtime-bound through the lambda capture). | Out of A.4 scope; queue as a separate refactor candidate if profiling identifies it. |
| Defer (chosen) | Honest about the API + math constraints; keeps the kernel bit-exact and unchanged. | Leaves the audit checklist row unticked. | Accepts the deferral cost in exchange for not shipping a half-working rewrite. |

## Consequences

- **Positive**: the kernel remains bit-exact against the existing
  cross-backend gate; no risk of regressing the
  `cross_backend_vif_diff.py --feature adm --backend sycl` audit.
  The `feedback_no_test_weakening` and `feedback_no_guessing`
  invariants are upheld — this PR ships no code that the dev host
  cannot validate.
- **Negative**: the post-oneAPI-2025.3 audit checklist item A.4
  remains unticked. Future contributors revisiting `oneapi-install.md`
  will see the same unchecked row and must read this ADR + the
  digest before re-attempting.
- **Neutral / follow-ups**:
  - [`oneapi-install.md`](../development/oneapi-install.md) gains a
    cross-link to this ADR next to the A.4 checklist row.
  - [ADR-0202](0202-float-adm-cuda-sycl.md) gains a 2026-05-08
    Status update appendix recording the SYCL DWT group_load
    investigation outcome (per task instruction; original body
    untouched per [ADR-0028](0028-adr-maintenance-rule.md)).
  - Research-0086 §A.4 is updated post-merge with the
    divisibility-blocker analysis as a `### A.4 update 2026-05-08`
    sub-section (handled in PR #464 directly via review comment;
    this PR does not touch the digest because it is not yet on
    `master`).

## References

- [Research-0086 §A.4](../research/0086-sycl-toolchain-audit-2026-05-08.md) —
  GO recommendation that motivated the implementation attempt.
- [PR #464](https://github.com/VMAFx/vmafx/pull/464) — the digest's
  own PR, where the §A.4 update lands as a review comment.
- [ADR-0202](0202-float-adm-cuda-sycl.md) — float ADM CUDA + SYCL
  twin parent ADR (gains a Status update appendix in this PR).
- [ADR-0028](0028-adr-maintenance-rule.md) — ADR immutability rule
  (Status updates as appendices, original body untouched).
- [`integer_adm_sycl.cpp`](../../core/src/feature/sycl/integer_adm_sycl.cpp)
  lines 206-351 (vert pass with manual cooperative SLM tile load),
  lines 354-457 (hori pass — no SLM tile, per-WI mirrored reads).
- `/opt/intel/oneapi/compiler/2025.3/include/sycl/ext/oneapi/experimental/group_load_store.hpp` —
  the in-tree header defining the `group_load` / `group_store`
  contract; functions at lines 447 (pointer + span), 462 (pointer +
  span store), 476 (scalar load), 490 (scalar store), 505 (vec load),
  519 (vec store). All overloads require
  `ElementsPerWorkItem ∈ {1, N}` with `total = WG_SIZE × ElementsPerWorkItem`.
- `feedback_no_guessing` — every `group_load` API claim in this ADR
  cites the in-tree header path + line number, never a hypothesised
  doc URL.
- `feedback_no_test_weakening` — the kernel stays untouched rather
  than ship a rewrite that cannot be bit-exact-validated on
  available hardware.
- Source: `req` (paraphrased: parent agent invoked the implementation
  follow-up to research-0086 §A.4; on detailed review of the SYCL
  ext API + the kernel's tile geometry, the GO recommendation was
  found unimplementable in its sketched shape and is deferred per
  this ADR).
