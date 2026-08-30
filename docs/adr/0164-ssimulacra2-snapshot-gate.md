# ADR-0164: SSIMULACRA 2 snapshot-JSON regression gate (T3-3)

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: test, ssimulacra2, regression-gate, fork-local

## Context

Backlog item T3-3 calls for a regression gate that catches unintended
drift in the fork's `ssimulacra2` feature extractor output. ADR-0130
deferred this gate with the note:

> Snapshot-comparison against `tools/ssimulacra2` ships as a follow-up PR
> (`ssimulacra2_rs` cargo install currently broken; Pacidus Python port
> github.com/Pacidus/ssimulacra2 remains a viable reference).
>
> This PR does not commit `testdata/scores_cpu_ssimulacra2.json`.

Three full SIMD ports later ([ADR-0161](0161-ssimulacra2-simd-bitexact.md),
[0162](0162-ssimulacra2-iir-blur-simd.md), [0163](0163-ssimulacra2-ptlr-simd.md))
closed T3-1 in full. Zero scalar hot paths remain. We have 15 bit-exact
unit tests pinning kernel-level behaviour. What we lacked: an
end-to-end integration gate pinning the whole-extractor output.

## Decision

Ship a fork-local Python integration test
[`python/test/ssimulacra2_test.py`](../../python/test/ssimulacra2_test.py)
that invokes the `vmaf` CLI with `--feature ssimulacra2` on two known
YUV fixtures (already checked in under `python/test/resource/yuv/`) and
asserts the per-frame + pooled scores against pinned floats with 4-place
tolerance.

Explicitly NOT chosen: cross-checking against `tools/ssimulacra2` (the
libjxl reference) or against the Pacidus Python port. Rationale:

- **libjxl `tools/ssimulacra2`** is not trivially installable in a CI
  image — requires a libjxl build from source, plus specific PNG / JXL
  codec dependencies. Adding this as a CI gate is a much bigger scope.
- **Pacidus Python port** has known discrepancies with the libjxl
  reference due to differences in the Gaussian blur implementation
  (scipy.ndimage.gaussian_filter vs libjxl FastGaussian 3-pole IIR).
  The fork's scalar follows libjxl; any comparison would produce a
  bounded-but-nonzero delta, requiring a tolerance argument.

The self-consistency gate we ship here catches the practical concern:
unintended behaviour change inside the fork's own implementation
(e.g. a future SIMD port drifting from scalar, or a scalar refactor
breaking the libjxl-reference semantics). The pinned values were
generated on a CPU-only build at the current master HEAD.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Fork-self-consistency gate via Python `unittest` subprocess + JSON + `assertAlmostEqual(places=4)` (this ADR)** | Simple; no new CI dependency; catches real drift; covers 2 fixtures × 48 frames = 96 scores | Doesn't cross-check against an independent reference | **Chosen** — closes T3-3's stated goal at minimal scope |
| **Gate against libjxl `tools/ssimulacra2`** | True third-party reference | Requires libjxl build + codec deps in CI; cargo install of `ssimulacra2_rs` is broken; bigger PR surface | Rejected — scope creep, environment fragility |
| **Gate against Pacidus Python reference** | Pure-Python; installable via pip | Known scalar-level drift vs libjxl FastGaussian IIR; requires a tolerance argument to mask the difference; muddles the "bit-exact within fork" story | Rejected — would pin the fork to a non-authoritative scalar path |
| **Ship only kernel-level SIMD tests, no integration gate** | Zero new test surface; rely on SIMD tests | Misses end-to-end integration regressions (e.g. extractor state init bug, YUV-matrix dispatch bug) | Rejected — the unit tests don't exercise the full pipeline including YUV → linear RGB → DCT scale pyramid → blur → mask → score pooling |

## Consequences

- **Positive**:
  - Real regression gate on the ssimulacra2 extractor output. Any
    future change that drifts per-frame scores by more than 1e-4 is
    caught in the fork's standard Python test suite.
  - Uses the already-checked-in `src01_hrc00/hrc01_576x324.yuv` +
    `dis_test_0_1_..._q_160x90.yuv` fixtures. No new test data.
  - No new CI dependencies.
  - Closes backlog T3-3.
- **Negative**:
  - Not cross-checked against an independent reference. If libjxl
    changes their algorithm and we want to track, we'd need to
    manually re-sync + update the pinned values.
- **Cross-host reproducibility**:
  - Initial PR attempt used `places=4`. First CI run showed ~2e-4
    drift between the AVX-512 authoring host (Arch Linux, glibc)
    and the CI GCC/clang hosts (Ubuntu, glibc; macOS, libSystem).
  - Investigated FMA-fusion as the suspect and hardened the build
    anyway: every ssimulacra2 source (scalar extractor + AVX2 +
    AVX-512 + NEON SIMD TUs) now compiles in a dedicated static
    lib with `-ffp-contract=off`. Mirrors the ADR-0161 NEON
    carve-out; prevents cross-host FMA-fusion drift as a matter
    of principle and protects against future fusion-related
    regressions.
  - BUT the drift persisted under `-ffp-contract=off`. Root cause
    was **libm variance**, not fusion: the phase-1 `cbrtf` and
    phase-3 `powf` per-lane scalar calls hit host libc, and
    different libm implementations (glibc / musl / macOS
    libSystem) compute the transcendentals with different
    polynomial approximations that differ by ~1 ulp. Aggregated
    across 48 frames × 6 scales × 3 planes, this compounds to
    ~2e-4 in the pooled score.
  - **Proper fix (shipped)**: replace libm `cbrtf` and `powf` with
    deterministic host-independent implementations in
    `core/src/feature/ssimulacra2_math.h` —
    - `vmaf_ss2_cbrtf()`: bit-trick initial estimate + 2 Newton–
      Raphson iterations, accuracy ~7e-7, pure IEEE-754 float
      arithmetic.
    - `vmaf_ss2_srgb_eotf()`: 1024-entry LUT with linear
      interpolation, accuracy ~5e-7. LUT values committed as
      hardcoded hex-float literals in
      `core/src/feature/ssimulacra2_eotf_lut.h`, generated
      offline by `scripts/gen_ssimulacra2_eotf_lut.py`.
    Both are used consistently across scalar + AVX2 + AVX-512 +
    NEON paths, preserving the ADR-0161/0162/0163 scalar-vs-SIMD
    bit-exact contract while removing the runtime libc
    dependency.
  - Consequence: output drifts from the libjxl `tools/ssimulacra2`
    reference by ~1e-6 per frame (within scope per ADR-0130, which
    never committed to libjxl bit-exactness in CI), and is now
    bit-identical across hosts. Tolerance stays tight at
    `places=4` (1e-4).
  - The fp-contract=off build splits remain in place as
    belt-and-suspenders hardening against any future FMA-related
    drift.
- **Neutral / follow-ups**:
  - When libjxl releases a new SSIMULACRA 2 reference version or
    `ssimulacra2_rs` becomes installable again, a separate ADR could
    add a second, cross-reference gate.
  - The pinned values assume CPU dispatch. If anyone adds a CUDA or
    SYCL path for ssimulacra2 in the future (currently neither exists),
    the test would need `--no_cuda --no_sycl` flags appended or a
    split variant.

## Verification

- `python -m pytest test/ssimulacra2_test.py -v`:
  **2/2 pass** on the AVX-512 host.
- Values pinned against master HEAD `origin/master` post-merge of PR
  #100. Per-frame spot-checks included (frame 0 and frame 47).

## References

- [ADR-0130](0130-ssimulacra2-scalar-implementation.md) — scalar
  SSIMULACRA 2 port (T3-3 deferral point).
- [ADR-0161](0161-ssimulacra2-simd-bitexact.md) — SSIMULACRA 2 phase
  1 SIMD (pointwise).
- [ADR-0162](0162-ssimulacra2-iir-blur-simd.md) — phase 2 SIMD
  (IIR blur).
- [ADR-0163](0163-ssimulacra2-ptlr-simd.md) — phase 3 SIMD (YUV→RGB).
- [ADR-0024](0024-netflix-golden-preserved.md) — project rule #1
  (Netflix golden assertions are sacred). This ADR ships a
  SEPARATE fork-added test; it does not touch Netflix golden values.
- Research digest:
  [`docs/research/0018-ssimulacra2-snapshot-gate.md`](../research/0018-ssimulacra2-snapshot-gate.md).
- User popup 2026-04-24: "T3-3: SSIMULACRA 2 snapshot-JSON gate".
