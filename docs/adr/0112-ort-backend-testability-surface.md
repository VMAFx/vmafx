# ADR-0112: Testability surface for `ort_backend.c` static helpers

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: dnn, testing, coverage

## Context

After [ADR-0111](0111-coverage-gate-gcovr-with-ort.md) landed (gcovr +
ORT in the coverage job), the per-file 85% gate exposed
`core/src/dnn/ort_backend.c` at 77.3% line coverage — short of the
threshold by ~8 percentage points (~31 lines of 397).

Auditing the uncovered lines showed three structurally unreachable
classes on a CPU-only ORT CI build, where "unreachable" means *no
combination of inputs to the public `libvmaf/dnn.h` surface can drive
the branch*:

1. **EP-attach success branches** (CUDA / OpenVINO:GPU / OpenVINO:CPU /
   ROCm). The CI ORT package is the stock CPU-only build, so every
   `try_append_*` call returns non-null `OrtStatus` and the success
   arms (`sess->ep_name = "CUDA"`, etc.) never execute. ~7 lines.
2. **ORT-API-failure branches.** Every `OrtStatus`-returning call has a
   failure path that releases the status and propagates `-EIO`. ORT
   does not fail these calls under normal use, and we do not have a
   fault-injection layer. ~15 lines.
3. **Internal-helper edge cases:**
    - `fp32_to_fp16` / `fp16_to_fp32` — the inf/nan, overflow,
      underflow, and subnormal arms. The existing `test_ep_fp16`
      round-trip uses values that stay in the normal exponent range
      (0.0, 1.0, -0.5, 256.0), so the edge arms never fire. ~13
      lines.
    - `resolve_name` — the positional-fallback `pos >= count` branch.
      `dnn_api.c::vmaf_dnn_session_run` validates `n_outputs <=
      sess->n_outputs` before reaching `vmaf_ort_run`, so the stricter
      check inside `resolve_name` is dead defence-in-depth. 1 line.
    - NULL-guard branches in `vmaf_ort_open` /
      `vmaf_ort_attached_ep` / `vmaf_ort_close` /
      `vmaf_ort_io_count` / `vmaf_ort_input_shape` / `vmaf_ort_run`.
      The `dnn_api.c` wrapper validates inputs before calling into
      `ort_backend`, so the guards inside `ort_backend` are
      defence-in-depth that never fires through the public API.
      ~7 lines.

Class 1 and 2 are not testable without either (a) shipping a
multi-EP ORT in CI (build-time and runtime cost: extra ~5–10 minutes
per coverage run, plus runners that lack the actual hardware would
fail at session creation, not at EP-attach), or (b) a fault-injection
layer that wraps the OrtApi vtable. Both are disproportionate for the
defence-in-depth they cover.

Class 3 is testable, but only by reaching the helpers directly. The
helpers are `static` in `ort_backend.c` and have no
publicly-callable proxy that exercises the edge values without going
through ORT itself.

## Decision

Add a private internal header `core/src/dnn/ort_backend_internal.h`
that exposes thin extern wrappers around the `static` helpers
(`fp32_to_fp16`, `fp16_to_fp32`, `resolve_name`). The originals
remain `static` so production call sites (`build_input_tensor`,
`copy_output_tensor`, `vmaf_ort_run`) keep their inlining. The
wrappers exist in both the `VMAF_HAVE_DNN` and stub branches of
`ort_backend.c` so a new test binary, `test_ort_internals`, links on
either build.

`test_ort_internals.c` covers Class 3:

- fp32→fp16 normal / inf / nan / overflow / underflow / subnormal
- fp16→fp32 normal / zero / subnormal / inf / nan
- resolve_name hit / miss / positional / out-of-range
- NULL-guard branches on every public-ish symbol in `ort_backend.h`
  (open / close / attached_ep / io_count / input_shape / run)

Plus the existing `test_ep_fp16.c` gets one new case
(`test_fp16_io_edge_values`) that drives the same fp16 conversion
edges through the *full* public-API round trip, so the integration
path is also covered, not only the isolated helpers.

Class 1 and 2 remain uncovered. The 85% gate must be evaluated
*after* Class 3 is closed; if `ort_backend.c` still cannot reach 85%,
the next move is to lower the per-file threshold for `ort_backend.c`
specifically (with a follow-up ADR documenting the EP-availability
constraint), not to add fault-injection or multi-EP CI.

## Alternatives considered

1. **Lower the per-file threshold for `ort_backend.c` to 75%, no
   refactor.** Faster, no source surface change, but loses the fp16
   conversion edge tests entirely — those edges are the most likely
   place a real bug would hide (subnormal handling, sign of zero,
   inf-vs-nan distinction). The user's "no skip-shortcuts" rule
   pushes against threshold-lowering as the *first* response.
   Rejected as the primary fix; kept as the *fallback* if Class 3
   coverage still leaves the file short.
2. **Add CUDA / OpenVINO ORT to the coverage CI build.** Covers EP-
   attach branches naturally, and makes the coverage gate more
   honest. Significant CI cost (extra runtime install, ~5–10 min
   build delta per run) and the runners do not have the matching GPU
   hardware, so EP attach itself may fail at session creation rather
   than at append-time. Net coverage gain: uncertain. Deferred until
   we genuinely need the EP-specific paths exercised in CI for
   correctness, not for coverage metrics.
3. **Fault-injection wrapper around the OrtApi vtable.** Covers Class
   2 by replacing the ORT API table with a mock that returns
   non-null status on demand. Substantial test infrastructure
   (~300+ LOC) and changes the `ort_backend.c` design to depend on
   an indirection. Rejected on cost; defence-in-depth branches do
   not justify it.
4. **Refactor: extract `fp32_to_fp16` / `fp16_to_fp32` /
   `resolve_name` into a separate `ort_helpers.c` translation unit.**
   Cleaner boundary than the wrapper-in-place approach, but moving
   ~73 lines (mostly covered) out of `ort_backend.c` actually
   *lowers* its coverage percentage even when the new TU is fully
   tested, because the moved lines were already above the file
   average. Rejected as counter-productive for the coverage metric.

## Consequences

**Positive:**

- `ort_backend.c` coverage rises by ~22 reachable lines (fp16 edges,
  resolve_name positional-out-of-range, NULL guards across
  `vmaf_ort_*`), pushing the file toward the 85% gate.
- The fp16 conversion edges now have direct unit tests, not just
  integration coverage through ORT — regressions in subnormal /
  inf-handling fail loudly with named asserts.
- The static-helper wrappers cost zero runtime (they delegate to the
  static originals; the compiler will inline both away in production
  builds where the test binary doesn't link).

**Negative:**

- Two extra symbols on the libvmaf binary
  (`vmaf_ort_internal_fp32_to_fp16`,
  `vmaf_ort_internal_fp16_to_fp32`,
  `vmaf_ort_internal_resolve_name`). They are namespaced and
  documented as test-only in the header; downstream callers that
  treat them as public API are misusing the surface.
- Adds a header (`ort_backend_internal.h`) under `core/src/`
  that is not part of the public include tree — slightly more
  layout to keep straight.

**Neutral:**

- EP-attach success branches and ORT-API-failure branches remain
  uncovered. This is documented and accepted; if coverage metrics
  ever need to honestly account for them, the move is to add a
  proper EP-availability matrix to CI, not to inflate coverage with
  symbolic tests.

## References

- [ADR-0111](0111-coverage-gate-gcovr-with-ort.md) — gcovr migration
  ORT in coverage job.
- `req` (paraphrased): user direction was to write tests for all 5
  critical files in this PR; on hitting the structural ceiling for
  `ort_backend.c`, user selected the recommended option to expose
  static helpers + add direct unit tests + write this ADR.
- Per-surface doc impact: this ADR is the documentation surface for
  the new internal header — `ort_backend_internal.h` is a private
  test-support surface, not a public-API surface, so no
  `docs/api/` entry is required.
