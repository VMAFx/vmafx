<!-- markdownlint-disable MD060 -->
# ADR-0135: Port Netflix#1424 — expose built-in VMAF model-version iterator

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: @lusoris, Claude
- **Tags**: api, upstream-port, correctness

## Context

Upstream PR [Netflix#1424][pr1424] (open, 1 commit, +32/-0) adds a
public iterator so callers can discover the list of built-in VMAF
model versions baked into the shared library without hard-coding them
or parsing the pkg-config output.

The iterator design is a standard opaque-handle cursor:

```c
const void *vmaf_model_version_next(const void *prev, const char **version);
```

Pass NULL on the first call, pass the previous return on subsequent
calls, stop when NULL is returned. `*version` is the OUT-param for
each model's version string.

The upstream patch has three latent defects that any reasonably
strict compiler / analyzer will flag and that the fork's
`make test` (ASan + UBSan) would trip on:

1. **NULL-pointer arithmetic (UB, C11 §6.5.6/9)** — when `prev ==
   NULL`, the upstream body executes both `if` branches because they
   are not joined by `else`:

   ```c
   if (!prev_model) out_model = &built_in_models[0];
   if (prev_model - built_in_models < BUILT_IN_MODEL_CNT)
       out_model = prev_model + 1;   /* NULL + 1: UB */
   ```

   The second branch performs pointer arithmetic on a NULL pointer,
   overwriting `out_model` with `(void *)0x1` on typical platforms.
   The caller then dereferences that as `VmafBuiltInModel *` — segfault.
2. **Off-by-one at end of iteration** — `BUILT_IN_MODEL_CNT` in the
   fork (same macro shape upstream) equals
   `sizeof(built_in_models)/sizeof(built_in_models[0]) - 1` because
   the array is terminated with a `{0}` sentinel. The upstream
   condition `prev_model - built_in_models < BUILT_IN_MODEL_CNT`
   admits the last real index (`CNT - 1`), so the next `+1` step
   lands on the sentinel and returns it. The sentinel has
   `.version == NULL`, which the caller treats as a valid model
   with a NULL name — crash on the next `strcmp`. The guard must be
   `idx + 1 < CNT`, not `idx < CNT`.
3. **const-qualifier mismatches in the test** — upstream's test uses
   `char *version` and `void *next`; the API takes `const char **`
   and `const void *`. `&version` is a `char **` passed where
   `const char **` is expected, which is *not* an allowed implicit
   conversion in C (C11 §6.5.16.1). Compiles only because upstream
   ignores the warning; blocks on the fork's `-Werror` legs.

The fork has built-in models behind two compile-time flags
(`VMAF_BUILT_IN_MODELS` and `VMAF_FLOAT_FEATURES`), and the iterator
must handle the zero-models case cleanly (callers still compile and
linker-resolve the symbol, it just returns NULL immediately).

## Decision

Port the upstream API surface verbatim (`const void *
vmaf_model_version_next(const void *prev, const char **version);`) so
downstream callers written against upstream work unmodified, but
correct the three defects during the port:

1. Add `else` between the two branches so the NULL case cannot fall
   through to pointer arithmetic.
2. Use `idx + 1 < BUILT_IN_MODEL_CNT` so iteration stops before the
   `{0}` sentinel.
3. Early-return `NULL` when `BUILT_IN_MODEL_CNT == 0` — a constant
   condition that the optimiser folds out when the models are
   compiled in.
4. Rewrite the test to use the correct const-qualified types and
   add an explicit "iteration visits every model exactly once"
   assertion (the upstream test only checked pointer-equality of
   `version` on each step, which would also pass if the iterator
   dropped entries).

Header doc-comment is expanded to Doxygen format to document the
NULL-on-first-call contract and the "not modified on end" OUT-param
semantics — the upstream one-line slash-slash comment is too thin
to prevent the same bugs from re-emerging in a caller.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port upstream verbatim (carry the two UB defects + test warnings) | Smallest diff from upstream; trivial to `/sync-upstream` | UBSan / ASan fail immediately on the first iteration of any test; clang-tidy flags the NULL-arith at rule `-Wnull-pointer-arithmetic`; the third defect blocks on `-Werror`. Fork's CI would reject | Not shippable under fork's quality gates |
| Redesign the API to an index-based cursor (`size_t *state`) | No opaque-pointer dance; no UB possible | Source-incompatible with upstream; `/sync-upstream` becomes a manual reconciliation instead of a trivial merge | Too divergent for a 32-line port |
| Expose `VmafBuiltInModel` publicly and return `const VmafBuiltInModel *` directly | Type-safe, no `const void *` casts | Leaks a type that's today entirely private to `model.c` (extern-string trick, compile-time-flagged entries). Expanding the public ABI is a bigger decision than this backlog item warrants | Chosen to keep `VmafBuiltInModel` private and match upstream's opaque-handle shape |
| Defer until upstream merges #1424 | No fork-local diff | PR has been open since 2024-12, no merge signal; Tiny-AI's future model-discovery UI wants this surface | Ship the corrected version now; upstream merge becomes a trivial conflict resolved in favour of fork |

## Consequences

- **Positive**: Public API for enumerating built-in model versions is
  available and behaves correctly at both iteration boundaries.
  Callers can now list models without hard-coding version strings.
  Test exercises both the iterator contract and a total-count
  invariant. Zero-models build configuration still works (iterator
  returns NULL).
- **Negative**: Fork-local diff from upstream (three semantic
  corrections + test rewrite + header-doc expansion). Next
  `/sync-upstream` after Netflix merges #1424 will need manual
  conflict resolution in `core/src/model.c` and
  `core/test/test_model.c`; the resolution is simply "keep
  fork version" — document in `rebase-notes.md`.
- **Neutral / follow-ups**:
  - Consider exposing `VmafBuiltInModel` as a small opaque struct
    with accessors (name-only, no `data_len` internals) in a
    follow-up ADR if a caller actually needs more than the version
    string. Not blocking.
  - CLI `vmaf --list-builtin-models` is now easy to build on top
    of this. Not in scope for this ADR.
  - User-discoverable public C API changed; `docs/api/` update
    needed in the same PR (ADR-0100 per-surface bar).

## References

- Upstream PR: [Netflix#1424 — Expose builtin model versions][pr1424]
- Upstream issue/use-case: same PR description — ffmpeg filter and
  downstream tooling want model discovery without hard-coded lists.
- Backlog: [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md) T4-6
- C11 undefined-behaviour reference: §6.5.6/9 (pointer arithmetic
  on NULL), §6.5.16.1 (assignment-qualifier rules).
- Source: `req` — user direction to ship Batch-A (T0-1 + T4-4/5/6) as
  one PR (2026-04-20 popup).

[pr1424]: https://github.com/Netflix/vmaf/pull/1424
