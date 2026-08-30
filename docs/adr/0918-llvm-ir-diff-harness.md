<!-- markdownlint-disable MD060 -->
# ADR-0918: LLVM IR diff harness for bit-exact SIMD paths

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: simd, build, ci, perf, diagnostics, fork-local

## Context

The fork's bit-exact SIMD invariants (ADR-0125, ADR-0138, ADR-0139) are
enforced today by score-level snapshot gates: a `vmaf` run on the
Netflix golden pairs, compared against a stored JSON. The gate is
correct but **diagnostically thin** — when it trips, the failure mode
is "score drifted by 4 ULP" with no signal about why. PR #339 and
PR #382 each took two review rounds because the root cause was
compiler-induced: clang's `-ffp-contract=on` (the default in newer
LLVMs) had silently fused `fmul + fadd` into `llvm.fma.v8f32`
intrinsics inside a path that the ADR-0138 fix had explicitly
disclaimed via `#pragma STDC FP_CONTRACT OFF`. The pragma was
honored on the original clang but partially ignored on the bumped
version.

The signal lives in the LLVM IR. If the IR before and after a
compiler bump differs in FMA emission, the floating-point semantics
change too. We need a check that runs at *build* time (cheap,
deterministic, no YUV decode) and points directly at the affected
function and intrinsic — not at a 4-ULP delta three minutes into a
test run.

## Decision

We will ship an opt-in `make ir-diff` target that compiles each
fork-added SIMD `.c` source listed in
`scripts/perf/ir-diff-config.yaml` with
`clang -O2 -mavx2 -mfma -emit-llvm -S`, extracts the configured
function bodies, normalises non-semantic noise (debug metadata,
attribute IDs, source paths), and diffs against a golden snapshot
under `testdata/ir-snapshots/<func>.ll`. Drift fails the gate with
a unified-diff report and an FMA-count delta line.

The harness is **on-demand, not a default CI gate**: it adds a clang
re-compile per SIMD file and we already pay a full meson build for
every PR. The dev runs it (a) after touching any SIMD source, (b)
after bumping the clang version in `dev/Containerfile` /
`.github/workflows/*`, or (c) when triaging a snapshot regression.
`make ir-diff-update` re-seeds the snapshots, with the same
"justify in the commit message" discipline as `/regen-snapshots`
for score JSONs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Snapshot LLVM IR per function (this ADR)** | Cheap to run; points directly at the function and intrinsic that drifted; tractable to read in code review; works without YUV fixtures | Snapshot updates needed on intentional changes; needs clang on PATH | Picked |
| Pin `-ffp-contract=off` repo-wide in meson | Globally suppresses the failure class | Lobotomises perf for SIMD paths that *want* `_mm256_fmadd_ps`; not a check, just a workaround; doesn't catch other compiler-induced drifts | Punishes the 95 % to defend the 5 %; doesn't generalise to non-FMA semantic shifts |
| Grep the `.s` (assembly) output instead of IR | Works without LLVM tooling | Assembly is target- and microarch-specific and noisy; instruction scheduling differs across clang minor versions even at fixed `-march` | Higher false-positive rate than IR; harder to normalise |
| Score-only gate (status quo) | Already in place; catches all real regressions eventually | Triggers late; doesn't localise the cause; cost ~3 min per CI loop | This is exactly what PR #339 / #382 already exercised — the slow diagnostic loop is the bug |
| Run as a default CI gate | Catches drift on every PR | Adds N clang compiles to every PR; most PRs don't touch SIMD; trades a rarely-needed gate for steady CI minute cost | Cheaper to keep it manual + advise running it in the PR template when SIMD is touched |

## Consequences

- **Positive**:
  - When a clang bump (`dev/Containerfile` rebuild, GitHub Actions
    runner image bump) introduces an FMA where there wasn't one, the
    dev sees `FMAs: ref=0 cur=9` on a named function in seconds —
    not "score drifted by 4 ULP" three minutes later.
  - Snapshot files under `testdata/ir-snapshots/` are checked into
    git, so the diff is reviewable line-by-line in the PR that
    intentionally regenerates it.
  - Cheap to add new functions to the config: append to the YAML,
    run `make ir-diff-update`, commit both.

- **Negative**:
  - Snapshots break on any intentional refactor of a covered
    function (extracting a helper, changing a loop bound). The
    `update` step plus commit-message justification mirrors
    `/regen-snapshots`; the same operator discipline applies.
  - Requires clang on PATH (already required by `make lint` and
    the dev container).
  - 8 IR snapshots add ~80 KB to the tree. Acceptable.

- **Neutral / follow-ups**:
  - Add the `make ir-diff` reminder to the PR template's SIMD
    section in a follow-up PR (not this one).
  - When AVX-512 paths land additional bit-exact-required entry
    points, extend the config to cover them — the harness is
    ISA-agnostic.
  - The harness currently uses a single `-O2 -mavx2 -mfma` invocation
    per source file. If we later add NEON or AVX-512 entries, the
    `cflags:` per-entry field in the YAML can override the defaults.

## References

- [ADR-0125 — MS-SSIM decimate SIMD bit-exactness](0125-ms-ssim-decimate-simd.md)
- [ADR-0138 — PSNR-HVS SIMD bit-exactness via `#pragma STDC FP_CONTRACT OFF`](0138-psnr-hvs-simd-bitexact.md)
- [ADR-0139 — SSIMULACRA2 IIR blur SIMD bit-exactness](0139-ssimulacra2-iir-blur-simd-bitexact.md)
- PRs #339, #382 — the two compiler-induced bit-exactness rounds
  that motivate this ADR.
- Source: `req` — paraphrased user direction that PR #339 and
  PR #382 took two review rounds each to fix compiler-induced
  FMA / FP-contract drift, and that an IR-level check would catch
  the same class of bug at build time before the score gate trips.
