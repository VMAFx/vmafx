# ADR-0957: SYCL kernel coverage round 4 (float_moment + SpEED + SSIMULACRA2)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude
- **Tags**: sycl, test, gpu, parity, kernel-coverage

## Context

Rounds 1 (PR #351), 2 (PR #376), and 3 (PR #446) drove SYCL parity
coverage from 12 % to 78 % (14 of 18 SYCL extractors). The round-3
ADR-0946 closed the float family + PSNR-HVS and explicitly deferred
four extractors to a round-4 backlog:

- `float_moment_sycl` — shares its TU with `integer_moment_sycl`
  (`core/src/feature/sycl/integer_moment_sycl.cpp`); the extractor
  is registered under the name `float_moment_sycl` and emits four
  headline features (`float_moment_ref1st/dis1st/ref2nd/dis2nd`).
- `speed_chroma_sycl` — SpEED chroma family (ADR-0567). 752-LOC
  SYCL TU at `core/src/feature/sycl/speed_chroma_sycl.cpp`.
  **Discovery during round-4 work:** the TU exists but is **not
  wired** into `sycl_feature_sources` in `core/src/meson.build`
  and the extractor symbol `vmaf_fex_speed_chroma_sycl` is **not
  registered** in `core/src/feature/feature_extractor.c`. The TU
  appears complete (no TODO/FIXME/stub markers) but ships as
  dormant scaffold.
- `speed_temporal_sycl` — SpEED temporal family (ADR-0567). 705-LOC
  SYCL TU at `core/src/feature/sycl/speed_temporal_sycl.cpp`.
  Same build-wiring + registration gap as `speed_chroma_sycl`.
  Carries the `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag — frame 0
  always emits `0.0`; the meaningful score would land at frame
  index 1.
- `ssimulacra2_sycl` — hybrid host + GPU port of the ADR-0201 Vulkan
  pipeline (ADR-0206). Multi-stage XYB + IIR + SSIM-combine pipeline
  whose float rounding accumulates past the places=4 baseline; the
  ADR-0214 `FEATURE_TOLERANCE` table assigns `5e-3` to it for the
  same reason.

The build-wiring + registration gap for the two SpEED twins is
out-of-scope for a kernel-coverage PR (it would change the
production extractor surface, not just test coverage). The two
tests are added in dormant form and surface a `[skip: ...not built
into libvmaf]` message that tracks the gap; they auto-activate as
real parity gates the moment a follow-up PR wires the TUs into
the build + registry.

Without a CPU↔SYCL parity gate, a stride / sub-group-mask /
coefficient drift in any of these kernels would silently corrupt
SpEED, float-moment, or SSIMULACRA2 columns on Intel-Arc CHUG
re-extracts; the cross-backend gate (ADR-0214) covers some of
these on Vulkan/lavapipe but not on the SYCL backend on Intel-Arc.

## Decision

Add four CPU vs. SYCL parity gates under `core/test/`, gated on
`enable_sycl`:

| Kernel | New test | Headline score(s) | Tolerance | Status |
| --- | --- | --- | --- | --- |
| `float_moment_sycl` | `test_sycl_float_moment_parity.c` | `float_moment_{ref,dis}{1st,2nd}` | `1e-4` (ADR-0214 default) | Active — extractor built + registered |
| `speed_chroma_sycl` | `test_sycl_speed_chroma_parity.c` | `Speed_chroma_feature_speed_chroma_{u,v,uv}_score` | `1e-4` (ADR-0214 default) | Dormant skip — TU not yet wired into build/registry |
| `speed_temporal_sycl` | `test_sycl_speed_temporal_parity.c` | `Speed_temporal_feature_speed_temporal_score` @ idx 1 | `1e-4` (ADR-0214 default) | Dormant skip — TU not yet wired into build/registry |
| `ssimulacra2_sycl` | `test_sycl_ssimulacra2_parity.c` | `ssimulacra2` | `5e-3` (ADR-0214 `FEATURE_TOLERANCE['ssimulacra2']`) | Active — extractor built + registered |

Each mirrors the rounds 1–3 pattern: 256x144 synthetic YUV420P
fixture, per-frame XOR pattern with frame-dependent salt, CPU + SYCL
feature extractor through the public `vmaf_use_feature` API with
`NULL` options dict (defaults match between CPU and SYCL for all four
kernels), parity assertion via `fabs(cpu - sycl) <= TOL`, skip-on-
no-device via `[skip: no SYCL device]` printf.

The SSIMULACRA2 fixture fills all three planes (not just luma) because
the pipeline consumes YUV → linear-RGB → XYB and chroma matters for
the headline score. The speed_temporal fixture submits two frames
(the TEMPORAL flag means frame 0 emits 0.0) and asserts at index 1.

The two SpEED tests additionally guard with
`vmaf_get_feature_extractor_by_name(...)` and emit
`[skip: <name> not built into libvmaf]` when the extractor is
absent — see the Context section above for the build-wiring gap
discovered during round-4 implementation. Each test auto-activates
as a real parity gate the moment a follow-up PR wires the SpEED
TUs into `core/src/meson.build` + `core/src/feature/feature_extractor.c`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Four per-kernel test files (chosen) | Mirrors rounds 2 + 3 layout; one TU per kernel keeps build parallelism and failure-isolation; reviewers can bisect a single failing kernel | Four new TUs vs. one combined | Round-2 / round-3 set the precedent; combining hides which kernel regressed |
| One combined `test_sycl_round4_parity.c` | Single TU = ~250 LOC vs. ~700 LOC across four files | Failure attribution becomes harder; `meson test --run-only` granularity is lost; breaks the AGENTS.md coverage matrix invariant (one row per parity test) | Loses the rounds 1–3 invariant from the AGENTS.md note in `core/src/feature/sycl/AGENTS.md` |
| Defer ssimulacra2 to round 5 (cover 3 kernels here) | Tighter PR; the ssimulacra2 gate uses a different tolerance, special-cases the fixture, and could land standalone | Leaves the headline SSIMULACRA2 SYCL gap open for another cycle; the per-kernel scaffold cost is the same whether bundled or split | Four kernels still fits comfortably in the 200–800 LOC bundle target |
| Drop `places=4` and use `5e-3` for all four | Single tolerance constant; easier to maintain | Hides drift in `float_moment_sycl`, `speed_chroma_sycl`, `speed_temporal_sycl` whose kernels are bit-tight per ADR-0214 default | ADR-0214 is per-feature; matching its `FEATURE_TOLERANCE` is the invariant |
| Extract a shared `test/sycl_parity_helpers.{h,c}` first | Eliminates the run_cpu / run_sycl boilerplate duplicated across all 12 rounds 1–4 tests | Touches 12 existing tests + adds a new TU; out of scope for a kernel-coverage PR | Tracked as a follow-up refactor; the per-test divergence (config dicts, multi-score, multi-frame) is small enough that the boilerplate cost is acceptable |

## Consequences

- **Positive**: SYCL parity *test* coverage rises from 78 % to
  **100 %** of the SYCL extractors enumerated in the round-3
  backlog (4 of 4). Every registered SYCL extractor (16 of 18,
  excluding the dormant SpEED scaffolds) now has an active
  CI-gated CPU↔SYCL parity test; the two unregistered SpEED twins
  carry dormant parity tests that activate automatically once the
  build wiring + registration follow-up lands. The round-4
  backlog row in `core/src/feature/sycl/AGENTS.md` is closed.
- **Negative**: Four new test executables increase `enable_sycl`
  build time by ~25 s and the test runtime by ~8 s on systems with
  a SYCL device. Systems without a SYCL device run the skip-path
  and add ~0.4 s to the suite.
- **Negative — discovered during round-4**: `speed_chroma_sycl`
  (752 LOC) and `speed_temporal_sycl` (705 LOC) ship as dormant
  scaffold — full SYCL TUs that are neither compiled into
  `libvmaf.{so,a}` nor registered with the extractor registry.
  This PR documents the gap and tracks it via dormant tests; the
  follow-up to wire them in is left to a dedicated PR (it changes
  the production extractor surface, not just test coverage).
- **Neutral / follow-ups**: The run_cpu / run_sycl boilerplate is
  now duplicated across 12 SYCL parity tests (rounds 1 + 2 + 3 + 4).
  A future PR may extract `test/sycl_parity_helpers.{h,c}` once the
  shape stabilises (rounds 1–4 already converge on the same six
  steps: init, import_state, use_feature, feed, EOS, score_at_index).
  Out of scope for this PR.

## References

- ADR-0214 — cross-backend numerical-parity gate (per-feature tolerance table)
- ADR-0219 — CHUG re-extraction trusted-column invariant
- ADR-0567 — SpEED SYCL kernels (chroma + temporal)
- ADR-0206 — SSIMULACRA2 CUDA + SYCL ports (hybrid host/GPU)
- ADR-0192 — GPU long-tail batch 3 (per-feature precision contracts)
- ADR-0868 — GPU-backend kernel-coverage gap audit
- ADR-0884 — SYCL kernel coverage round 2
- ADR-0946 — SYCL kernel coverage round 3
- ADR-0108 — fork-local deep-dive deliverables rule
- PR #351 — SYCL kernel coverage round 1
- PR #376 — SYCL kernel coverage round 2
- PR #446 — SYCL kernel coverage round 3
- Research digest:
  `docs/research/0957-sycl-kernel-coverage-round4-2026-05-31.md`
- Source: `req` — operator brief 2026-05-31 ("SYCL kernel coverage
  round 4 — close the last 4 uncovered SYCL kernels").
