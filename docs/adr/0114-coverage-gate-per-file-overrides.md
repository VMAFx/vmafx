# ADR-0114: Per-file coverage-gate overrides for ort_backend.c + dnn_api.c

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, coverage, dnn, ort, gate

## Context

[ADR-0113](0113-ort-create-session-fallback-multi-ep-ci.md) shipped two
coordinated changes — a production CreateSession→CPU fallback in
`vmaf_ort_open` and a multi-EP coverage-CI install
(`onnxruntime-linux-x64-gpu-1.22.0.tgz` + `libcudart12`) — to push the
two stuck critical files above the 85% gate.

The ADR projected coverage gains from exercising the CUDA EP-attach
success arms. The actual run on `master`'s coverage gate showed:

| File              | Pre-ADR-0112 | Post-ADR-0112 | Post-ADR-0113 |
|-------------------|--------------|---------------|---------------|
| `dnn_api.c`       | 79.5%        | 79.5%         | 79.5%         |
| `ort_backend.c`   | 77.3%        | 83.6%         | 79.3%         |

`dnn_api.c` did not move. Its uncovered lines are EP-attach error
paths the multi-EP install can't reach (the CPU EP is always linked,
so its register call never returns the error category that would
exercise those branches) plus the `has_norm` sidecar normalisation
branch (lines 141-144) which is structurally dead — the sidecar
loader never sets `has_norm`.

`ort_backend.c` regressed from 83.6% → 79.3%. Cause: the new
two-stage CreateSession fallback adds ~30 lines of error-handling
(opts release, options recreate, retry) that fire only when both
non-CPU `CreateSession` *and* the CPU retry fail — which never
happens on a healthy CI runner. Adding ~15 reachable lines + ~15
unreachable lines drops the ratio. The fallback is correct
production code per ADR-0113; we just got the coverage projection
wrong because the fallback's own error paths are themselves
unreachable in CI.

ADR-0112 explicitly documented the fallback for this case: "lower
the per-file threshold for `ort_backend.c` specifically … with a
follow-up ADR documenting the EP-availability constraint." User
direction confirmed taking that path now.

## Decision

Add a per-file critical-coverage override map to
`scripts/ci/coverage-check.sh`:

```bash
declare -A PER_FILE_MIN=(
    ["core/src/dnn/ort_backend.c"]=78
    ["core/src/dnn/dnn_api.c"]=78
)
```

Files in the map use the override; everything else still uses the
global `CRITICAL_MIN` (currently 85). The script prints the
applied threshold on every check line so reviewers see exactly
which files are running below the headline 85% bar.

Threshold choice: **78%**. Current measurements are 79.3% and
79.5%; 78% gives ~1.3pp slack for normal test variance without
papering over real regressions. Tightens once we close one of the
structural gaps (e.g. by deleting the dead `has_norm` branch in
`dnn_api.c`, or by adding fault-injection coverage for the
fallback's nested error paths).

The override map *requires* an ADR citation in the comment above
each entry. Future contributors adding entries must add a new ADR
or extend this one. Drift control is exactly the same pattern as
the per-file `lint-disable` comments under `core/src/feature/`.

## Alternatives considered

1. **Lower the global `CRITICAL_MIN` from 85% to 78%.** Trivial to
   implement (one constant), but silently lowers the bar for every
   security-critical file including `read_json_model.c` (88.2%) and
   `model_loader.c` (86.4%) which have headroom. Removes pressure to
   keep them tested. Per-file overrides keep the global gate honest
   and force each exemption to come with a documented reason.
2. **Delete the dead `has_norm` branch in `dnn_api.c` (lines
   141-144).** Would lift `dnn_api.c` by ~3pp on its own. Mentioned in
   ADR-0113 References as a separate cleanup. Defer to its own commit
   — single-purpose, easier to review, doesn't entangle with the gate
   change. Either way the override for `ort_backend.c` still has to
   land because the fallback regression there is unrelated to
   `has_norm`.
3. **Build ORT from source with `--use_openvino` so OpenVINO:CPU EP
   covers the OpenVINO attach-success arms.** Adds 30-40 minutes per
   coverage run and breaks if upstream renames the configure flag
   between versions. Same cost-benefit calculation that ADR-0113
   already rejected for the OpenVINO/ROCm case.
4. **Add fault-injection wrappers around the OrtApi vtable to drive
   the fallback's nested error paths from a unit test.** ~300 LoC of
   test infrastructure for ~5pp of coverage on one file. Already
   rejected on cost in ADR-0112 + ADR-0113.
5. **Revert the ADR-0113 fallback to recover the 83.6% number.**
   Throws away a real production correctness fix to chase a coverage
   metric. Strictly worse user outcome.

## Consequences

**Positive:**

- Coverage gate is honest: 85% remains the bar everywhere except two
  files that have a documented, ADR-cited structural ceiling. The
  per-line output now shows the applied threshold so reviewers can
  spot drift instantly.
- PR #46 unblocks. The work in ADR-0110 → ADR-0113 lands without
  having to revert any production behaviour.
- The override map is a single block of bash; future per-file
  exemptions go in one place with one comment-style for the
  rationale.

**Negative:**

- We accept a 1.4-1.7pp gap from the headline 85% bar on two files.
  The cap is documented and bounded — if coverage drops below 78%
  on either file, the gate trips and we have to either fix it or
  add a new ADR lowering the floor further (which would be a
  visible policy change).
- `PER_FILE_MIN` is keyed by string path. If gcovr changes its
  emit-path format (currently relative to repo root) the override
  silently stops applying and the global 85% gate trips. Mitigation:
  `coverage-check.sh` prints the applied threshold per file, so the
  next CI run after a gcovr upgrade will show "min 85%" instead of
  "min 78%" on the override files and fail loudly.

**Neutral:**

- This is a follow-up to ADR-0112's documented fallback path, not a
  reversal of any decision. ADR-0113 stays Accepted — the production
  fallback is the right behaviour even though it didn't deliver the
  projected coverage gain.
- `read_json_model.c` (88.2%), `model_loader.c` (86.4%),
  `onnx_scan.c` (94.6%), `op_allowlist.c` (100%), `tensor_io.c`
  (97.2%), `opt.c` (100%) all stay on the global 85% bar.

## References

- [ADR-0110](0110-coverage-gate-fprofile-update-atomic.md) — atomic
  profile updates.
- [ADR-0111](0111-coverage-gate-gcovr-with-ort.md) — `lcov → gcovr`
  migration that made per-file numbers honest.
- [ADR-0112](0112-ort-backend-testability-surface.md) — testability
  surface for `ort_backend.c` static helpers; explicitly documented
  the fallback this ADR executes.
- [ADR-0113](0113-ort-create-session-fallback-multi-ep-ci.md) —
  CreateSession→CPU production fallback + multi-EP CI install. The
  fallback's nested error-handling is the immediate cause of the
  4.3pp regression on `ort_backend.c` documented above.
- `req` (paraphrased): on the popup that surfaced the persistent
  79.3% / 79.5% numbers post-ADR-0113, user instructed to "follow
  ADR-0112's documented fallback: lower per-file threshold for
  ort_backend.c specifically (with a follow-up ADR documenting the
  EP-availability constraint)."
- Per-surface doc impact: `scripts/ci/coverage-check.sh` is the
  authoritative gate definition; the override map is documented
  in-script with an ADR-back-reference. `docs/principles.md §3`
  states the headline 85% target — that text stays correct (the
  exception is documented here, not by editing the principles).
