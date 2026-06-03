# ADR-0888: Pyright strict audit of fork-local Python packages

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: Lusoris
- **Tags**: ai, mcp, python, type-safety, ci

## Context

PR #366 ran `mypy --strict` on the three fork-local Python trees
(`ai/src`, `mcp-server/vmaf-mcp/src`, `tools/vmaf-tune/src`) and
shipped a first wave of annotation tightening. Pyright is a separate
type-checker with different (and in several axes stricter) inference
rules: `TypeGuard`, `NewType` narrowing, `ParamSpec`, generic variance,
`Optional` member access through `raise`/early-return guards, and
`reportUnnecessaryComparison` / `reportUnnecessaryIsInstance`. The
fork already pays for one type-checker pass; running the second checker
costs only the diff in errors and surfaces a different class of bug.

The baseline pyright strict run on `master` (387839e) found
**1,688 strict errors** across the three packages. Most are
transitive `reportUnknown*Type` cascades from third-party deps without
stubs (torch, scipy, optuna, onnxruntime, pyarrow) — noise, not bugs.
A long tail of ~30 errors are real bugs or type-annotation lies that
mypy does not flag.

## Decision

Run pyright in strict mode on the same three trees as PR #366. Filter
out the third-party-stub-cascade noise (it does not point at fork
code defects). For the high-signal residue, fix the bugs whose
mechanical refactor is bounded and unambiguous; defer the
Protocol/variance reshapes that would cascade through public APIs.

The first pass targeted ~12 sites and lands them in one PR:

| Package      | Pyright strict baseline  | After this PR  | Δ       |
| ------------ | -----------------------: | -------------: | ------: |
| `ai/src`     | 370                      | 306            | -64     |
| `mcp/src`    | 61                       | 61             | 0       |
| `tune/src`   | 1,257                    | 1,236          | -21     |

The mcp count is unchanged: every remaining mcp pyright finding is in
`server.py`, owned by PR #366; this audit deliberately avoids files
that PR overlaps to preserve a clean rebase merge.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Pyright on every fork-local Python tree | broader coverage | huge cascade with third-party stubs, noisy first PR | scope it to the three trees PR #366 already strict-checks |
| Wait for PR #366 to merge first | avoids PR-overlap discipline | blocks on review queue; pyright finds real bugs PR #366 misses | run in parallel, skip overlapping files |
| Fix every pyright finding | maximum cleanliness | several findings are stub bugs (torch.clamp, scipy PearsonRResult.statistic) — the fix is upstream, not in the fork | focus on fork-code bugs; suppress stub-noise with cast/assert |
| Add `pyright` to CI | gate enforcement | second type-checker run cost; coordination with PR #366 | follow-up after both audits land |

## Consequences

- **Positive**:
  - Closes 12 real bug or annotation-lie sites pyright caught and
    mypy missed: undefined-`Tensor`, ORT-result narrowing, optuna
    optional-import access, two always-true Optional comparisons,
    one missing-Protocol-field (`CodecAdapter.presets`), two
    Optional-narrowing-through-raise gaps, scipy stats result access.
  - Establishes pyright strict as a runnable local gate via
    `pyrightconfig.audit.json` (untracked) — operators can re-run
    `pyright -p pyrightconfig.audit.json --outputjson <pkg>/`.
- **Negative**:
  - Two type-checker runs in CI is duplicate compute. Deferring
    that decision (mypy vs. pyright in CI) until PR #366 and this
    PR have both merged.
  - Some fixes are `cast`/`assert` boilerplate rather than reshaping
    the underlying type to be inferable. The audit prefers low-risk
    local fixes over public-API surgery.
- **Neutral / follow-ups**:
  - The bigger cleanups (CodecAdapter Protocol read-only / variance
    audit, ORT-result generic narrowing across the package, scipy
    stats stub vendor) want their own scoped PRs.
  - Codec adapter `presets` is now declared on the Protocol; if a
    future adapter omits it, pyright will flag at the registry.

## References

- PR #366 — `mypy --strict` audit of the same three trees (parent work).
- Research digest: `docs/research/pyright-strict-audit-2026-05-30.md`.
- Pyright strict-mode docs: <https://microsoft.github.io/pyright/#/configuration>
- Source: `req` — user request to run pyright strict to catch what mypy
  missed in TypeGuard / NewType / ParamSpec / generic variance, with
  the constraint to avoid file overlap with PR #366.
