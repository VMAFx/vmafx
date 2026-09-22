<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1289: The three SSIMULACRA2 HIP host functions may be split; their no-split citations are withdrawn

- **Status**: Proposed
- **Date**: 2026-09-22
- **Deciders**: Lusoris (pending — see References)
- **Tags**: hip, agents, code-quality, ci

## Context

`core/src/feature/hip/ssimulacra2_hip.c` carried an inline
`NOLINTNEXTLINE(readability-function-size)` on three functions, each with a
prose claim that the function must not be split:

| Function | Claim | Length on `origin/master` |
| --- | --- | --- |
| `ss2h_picture_to_linear_rgb()` | "Verbatim port of `ssimulacra2.c::picture_to_linear_rgb`. Splitting would break the line-for-line scalar-diff audit trail" | 75 LOC |
| `ss2h_run_scale_gpu()` | "Splitting would obscure the dispatch sequence required for parity audit" | 92 LOC |
| `extract_fex_hip()` | "Per-scale orchestration mirrors the CUDA extract loop. Splitting would obscure the dispatch ordering required for parity audit" | 68 LOC |

Each cited ADR-0141 §2 — the rule that `// NOLINT` is reserved for cases where
refactoring would break a load-bearing invariant — plus the ADR-0278 sweep
closeout.

Two things force a decision.

First, praetor's HISS-04 gate has a **touched-file clean rule** with no NOLINT
carve-out: any file a change touches must report zero infractions, whatever the
baseline says. The HISS-21 burn-down for `core/src/feature/hip/**` has to touch
this file — it removes the seven `goto` sites that violate HISS-01 — so keeping
all three functions at their current length means the branch cannot land the
HISS-01 fix at all. `praetorctl audit -base origin/master` reports exactly that:
three `touched file must be clean` findings, all in this file.

Second, the claim itself does not hold up. The fork's evidence that the HIP
SSIMULACRA2 twin matches the CPU scalar path is machine-checked, not read off a
diff: `core/test/test_hip_ssimulacra2_parity.c` compares device output against
the CPU reference on every `--suite gpu` run, and
`scripts/ci/cross_backend_parity_gate.py` carries an explicit `ssimulacra2`
tolerance of 5e-3 under ADR-0214. A line-for-line diff against
`core/src/feature/ssimulacra2.c` is a human-review aid on top of that gate, and
it was already approximate: the HIP copy renames every symbol (`ss2h_*`), reads
through `ss2h_read_plane()` instead of `read_plane()`, and drops the CPU
version's dead `ky` / `kcb_r` / `kcr_g` assignments.

## Decision

We will treat the three citations above as **withdrawn**, and allow those three
functions to be split into `static` helpers in the same translation unit.
ADR-0141 §2 itself is unchanged and is not superseded: the general rule still
stands, and this ADR records only that the specific factual claim these three
comments made — that splitting breaks a load-bearing invariant — is not true for
them, because the parity evidence is a test rather than a diff.

The split boundaries that replace them are `ss2h_yuv_primaries()`,
`ss2h_upload_xyb()`, `ss2h_download_blurred()` and
`ss2h_downsample_for_next_scale()`. Each is lifted at a statement boundary, is
`static` in the same TU, and copies its literals and ordering verbatim; none
crosses an arithmetic expression, and the ADR-1205 / ADR-0891 `fmaf()` chain in
`ss2h_picture_to_linear_rgb()` is untouched. With the split in place
`readability-function-size` no longer fires on any of the three, so their
`NOLINTNEXTLINE` lines are removed rather than relocated.

The CPU source (`core/src/feature/ssimulacra2.c::picture_to_linear_rgb`) and the
CUDA twin (`ss2c_picture_to_linear_rgb`, `ss2c_run_scale_gpu`,
`extract_fex_cuda`) keep their own citations until their own HISS-21 slices land.
The CPU one is a different case and stays: its comment records that four SIMD
ports (`avx2` / `avx512` / `neon` / `sve2`) are bit-exact against that exact
scalar body, so splitting it would force matching splits in four more files.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Split the three functions and withdraw the citations (chosen)** | Keeps the HISS-01 `goto` removal in this file; removes five inline suppressions instead of adding any; the file passes the touched-file rule; parity stays gated by the test that actually proves it | HIP's host helpers no longer line up statement-for-statement with the CPU and CUDA copies, so the three-way read is harder until the CUDA slice mirrors the same boundaries | **Decision** — the touched-file rule admits no alternative that keeps the HISS-01 fix |
| Restore the verbatim structure and drop `ssimulacra2_hip.c` from the HISS-21 slice | Citations stay literally true; no ADR needed | Abandons a completed HISS-01 fix (seven `goto` sites) and leaves four HISS-04 findings, 11 in total, to preserve a comment; HISS-01 is a hard invariant, a prose aid is not | Rejected — trades a real invariant for a documentation convenience |
| Restore the verbatim structure and take the `-touched-debt-delta-reason` escape hatch | Smallest diff | Suppresses the gate rather than satisfying it; explicitly out of bounds for this work | Rejected |
| Split the CPU and CUDA copies the same way so the three-way diff realigns | Honours the invariant's purpose instead of withdrawing it | Pulls `core/src/feature/ssimulacra2.c` and four SIMD ports into the touched set, each of which must then be fully clean; an order of magnitude more risk than the change it serves | Rejected — scope |

## Consequences

- **Positive**: `core/src/feature/hip/ssimulacra2_hip.c` reports zero praetor
  infractions and carries no `readability-function-size` suppression. The
  HISS-01 `goto` removal lands with the rest of the slice.
- **Negative**: the HIP host code diverges structurally from its CPU and CUDA
  twins until the CUDA slice lands. `core/src/feature/hip/AGENTS.md` records the
  helper boundaries so the CUDA slice can mirror them.
- **Neutral / follow-ups**:
  - When `core/src/feature/cuda/ssimulacra2_cuda.c` gets its own HISS-21 slice,
    mirror these four helper names and boundaries so the twins read against each
    other again.
  - `docs/rebase-notes.md` records the boundaries for conflict resolution.
  - This ADR is **Proposed**, not Accepted: it was written by an agent with no
    channel to the repository owner. It needs sign-off before the branch merges;
    rejecting it means falling back to the second option in the table above.

## References

- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file cleanup rule and
  the §2 NOLINT carve-out this ADR narrows (not supersedes).
- [ADR-0278](0278-t7-5-nolint-sweep.md) — the sweep closeout the withdrawn
  citations pointed at.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the cross-backend parity gate that
  actually enforces HIP-vs-CPU agreement, with the `ssimulacra2` 5e-3 tolerance.
- [ADR-1142](1142-whole-codebase-standards.md) — whole-tree standards, the reason
  the touched-file rule reaches this file at all.
- Source: audit blocker recorded against branch `chore/hiss21-core-src-hip`,
  2026-09-22: "You split a function whose own inline citation forbids splitting
  it ... Restore the verbatim structure and put the comment back where it
  belongs, or supersede the ADR — but the code and the citation must agree."
  No user popup was taken; no `req` or `Q<r>.<q>` citation exists for this
  decision, which is why the Status is Proposed.
