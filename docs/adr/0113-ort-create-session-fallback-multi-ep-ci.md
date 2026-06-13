# ADR-0113: ORT CreateSession fallback to CPU + multi-EP CI install

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: dnn, ci, coverage, ort

## Context

After [ADR-0112](0112-ort-backend-testability-surface.md) closed Class 3
(internal-helper edges + NULL guards) for `core/src/dnn/ort_backend.c`,
coverage rose 77.3% → 83.6% — still 1.4 percentage points below the 85%
critical-file gate. `dnn_api.c` was at 79.5% (5.5pp short) for the same
underlying reason: EP-attach success branches and ORT-API-failure
branches stayed unreachable on a CPU-only ORT build. ADR-0112 had
documented the fallback as "lower the per-file threshold for
`ort_backend.c` specifically", noting that the runner-up — ship multi-EP
ORT in CI — was deferred because runners lack GPU hardware (so EP
attach itself "may fail at session creation rather than at
append-time").

User direction was to take the multi-EP path anyway and see what
shakes out. Implementation surfaced the exact failure mode ADR-0112
warned about, plus a previously-hidden production correctness gap.

**The discovery.** With `onnxruntime-linux-x64-gpu-1.22.0.tgz` installed
plus `libcudart12` (the CUDA 12 user-space runtime, no driver), the
CUDA execution-provider register call (`SessionOptionsAppendExecution
Provider_CUDA`) succeeds — ORT can `dlopen
libonnxruntime_providers_cuda.so` and the CUDA runtime libs — but
`CreateSession` then fails because there is no actual GPU device
visible to the runtime. The pre-existing code path returned `-EIO` at
that point, even though the CPU execution provider is *always* linked
into the same ORT build and could have served the request.

This was a real production correctness gap, not a CI-only artifact.
Anybody building libvmaf against `onnxruntime-gpu` and running on a
node without an NVIDIA GPU got hard `vmaf_dnn_session_open` failures —
even when their `VmafDnnConfig.device = AUTO` explicitly authorised
falling back to CPU.

## Decision

Two coordinated changes:

1. **Production CreateSession-time fallback (`ort_backend.c`).** When
   `CreateSession` returns a non-null `OrtStatus` *and* `sess->ep_name
   != "CPU"` (i.e., we attached a non-CPU EP), release the failed
   session-options object, recreate it with no EP appended (CPU is the
   default linked EP), set `sess->ep_name = "CPU"`, and retry
   `CreateSession`. If the retry also fails, propagate `-EIO` as
   before. The `intra_op_threads` setting from `VmafDnnConfig.threads`
   is re-applied to the recreated options so user thread tuning is
   preserved across the fallback. `vmaf_ort_attached_ep()` reports the
   actual bound EP after fallback ("CPU"), so callers can detect the
   degraded mode.

2. **Multi-EP CI install in the coverage gate.** The Coverage gate
   workflow swaps `onnxruntime-linux-x64-${V}.tgz` for
   `onnxruntime-linux-x64-gpu-${V}.tgz` and adds `apt install
   libcudart12` (with `nvidia-cuda-toolkit` as a fallback if the
   runtime metapackage is unavailable). This exercises the CUDA-EP
   attach-success arms in `ort_backend.c` (and the new fallback path)
   without requiring an actual GPU runner. The Tiny-AI suite stays on
   the CPU-only build so its hardware assumptions don't shift.

Behaviour on the happy path (real GPU present, CUDA EP both registers
*and* initialises) is unchanged — the new code only runs when
`CreateSession` returns non-null status, which never happens in that
case. The fallback strictly improves degraded-mode behaviour.

## Alternatives considered

1. **Lower per-file thresholds for `ort_backend.c` and `dnn_api.c`
   (the ADR-0112 documented fallback).** Faster to implement, requires
   no production behaviour change, but leaves the EP-attach success
   arms permanently unreachable in CI and silently accepts the
   structural ceiling. User explicitly redirected to the multi-EP path
   even after seeing the 1.4pp / 5.5pp overshoot estimate. Kept as a
   second-tier fallback if the multi-EP install in CI yields less
   coverage than projected (e.g. if `libcudart12` isn't loadable on
   ubuntu-latest at all and the EP register call still fails at the
   `dlopen` layer).
2. **Rewrite tests to tolerate `-EIO` from `vmaf_dnn_session_open`
   when CUDA EP attached but session creation failed.** Same
   coverage gain (the EP-attach success arm fires before the failure)
   but leaves the production correctness gap in place — and degrades
   the test surface from "AUTO must succeed" to "AUTO may succeed or
   fail with -EIO depending on the runtime environment", which is
   exactly the kind of soft assertion that lets real bugs hide.
3. **Build ORT from source with `--use_openvino --use_cuda` so
   OpenVINO:CPU EP is available end-to-end.** Would cover OpenVINO:CPU
   without any GPU hardware (OpenVINO's own CPU runtime is
   self-contained). Build time +30–40 min plus complex CCache setup
   and breaks if upstream ORT drops `--use_openvino` flags between
   versions. Disproportionate for what it covers.
4. **Add a fault-injection wrapper around the OrtApi vtable.**
   ADR-0112 already considered and rejected this on cost
   (~300+ LOC of test infrastructure). Same reasoning applies here.

## Consequences

**Positive:**

- `vmaf_dnn_session_open` no longer returns `-EIO` in the realistic
  production scenario "ORT was built with CUDA EP, this host has no
  GPU". Sessions now open cleanly on CPU. The legacy soft contract
  ("AUTO never errors on a CPU-only build") now holds for "AUTO never
  errors on any build where at least the CPU EP is linked" — i.e.,
  every supported build.
- Coverage of `ort_backend.c` should rise: the CUDA EP-attach success
  arm (`sess->ep_name = "CUDA"`), the CreateSession failure path, the
  ReleaseSessionOptions / CreateSessionOptions retry path, and the
  fallback `sess->ep_name = "CPU"` reset all execute now under the CI
  multi-EP install.
- Existing `test_auto_falls_through_to_cpu` and
  `test_explicit_cuda_graceful_fallback` keep passing without
  modification — the contract they encode ("EP request does not fail
  open") is now honoured via session-creation fallback instead of
  append-time fallback. The semantic is the same; the path changed.
- Callers wanting to detect the degraded mode read
  `vmaf_dnn_session_attached_ep()` and observe `"CPU"` after a
  CUDA-with-no-device fallback. No new API surface.

**Negative:**

- Coverage CI step now downloads ~500 MB instead of ~200 MB
  (`onnxruntime-gpu` vs `onnxruntime`) and `apt install libcudart12`
  adds ~30 s. Net delta: estimated +2–3 minutes per coverage run.
- The fallback path adds ~30 lines of code to `vmaf_ort_open` and one
  more state transition in the EP-binding flow. Future refactors that
  rework EP selection must preserve the fallback semantic (or
  consciously remove it with an ADR).
- If a future ORT release changes how `CreateSession` reports CUDA
  initialisation failures (e.g. wraps them in a different OrtStatus
  category), the fallback may not trigger and we silently regress to
  the old hard-failure behaviour. Mitigation: the existing
  `test_explicit_cuda_graceful_fallback` test will catch that
  regression on the CI multi-EP install.

**Neutral:**

- ROCm and OpenVINO EP attach-success arms remain uncovered. The
  CI install does not bring those EPs (`onnxruntime-gpu` ships only
  CPU + CUDA + TensorRT). If those branches need direct coverage in
  the future, build-from-source is the only realistic path.
- `dnn_api.c`'s `has_norm` sidecar normalisation branch (lines
  141-144) remains structurally dead — the sidecar loader never sets
  `has_norm`. This is a separate cleanup; if removed entirely it
  would push `dnn_api.c` coverage up by ~3 percentage points without
  any test work.

## References

- [ADR-0110](0110-coverage-instrumentation-atomic-update.md) — gcov
  atomic profile updates.
- [ADR-0111](0111-coverage-gate-gcovr-with-ort.md) — gcovr migration
  and ORT install in coverage CI.
- [ADR-0112](0112-ort-backend-testability-surface.md) — internal
  helpers exposed for unit tests; documented multi-EP CI as a
  deferred alternative this ADR now executes.
- `req` (paraphrased): user directed taking the Multi-EP CI route
  even after seeing the deferred-status note in ADR-0112; on the
  follow-up popup that surfaced the CreateSession-failure mode, user
  selected the production CreateSession fallback as the response,
  with this ADR documenting the production behaviour change.
- Per-surface doc impact: production correctness change to
  `core/src/dnn/ort_backend.c` is user-observable for downstream
  consumers building against `onnxruntime-gpu` on hosts without
  NVIDIA hardware. The behavioural change is a strict relaxation
  (errors → success), so no new public-API entry is needed; the
  existing `vmaf_ort_attached_ep` accessor is sufficient to detect
  the degraded mode.
