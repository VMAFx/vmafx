# ADR-0976: Remove dead has_norm sidecar fields and fix extract_string_array leak

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: dnn, sidecar, cleanup, leak, security

## Context

A deep audit of `core/src/dnn/` (PR-scope: `dnn_api.c`, `ort_backend.c`,
`model_loader.c`, `onnx_scan.c`, `op_allowlist.c`, `tensor_io.c`,
`dnn.h`) surfaced two real bugs in the sidecar-JSON handling path.

**(1) Dead `has_norm` / `expected_min` / `expected_max` consumer
branches.** `VmafModelSidecar` (in `core/src/dnn/model_loader.h`)
carries six fields — `norm_mean`, `norm_std`, `has_norm`,
`expected_min`, `expected_max`, `has_range` — that the sidecar JSON
parser (`vmaf_dnn_sidecar_load`) never populates. None of the shipped
trainers (`ai/scripts/*`) emit the corresponding JSON keys either —
the canonical pipeline bakes per-frame normalisation directly into
the ONNX graph at export time. The three consumer call sites
(`dnn_api.c` lines 174 + 214 in `vmaf_dnn_session_run_luma8` /
`vmaf_dnn_session_run_plane16`, and `libvmaf.c` ~line 1175 in the
in-process NCHW dispatcher) read `has_norm` to decide whether to
apply a luma scaler — since the field is always false (memset-zeroed
and never written), every consumer branch silently selected
mean=NULL / std=NULL anyway. The dead branches were already
documented in [ADR-0114](0114-coverage-gate-per-file-overrides.md)
Alternatives §2 as a deferred cleanup that would recover ~3pp of
coverage on `dnn_api.c`.

Risk if left in place: a future contributor reading the consumer
code reasonably concludes the runtime supports sidecar-driven
normalisation, ships a sidecar JSON with `norm_mean` / `norm_std`,
and observes silently-wrong inference output. The producer /
consumer mismatch is a latent contract violation.

**(2) `extract_string_array` leaked partial allocations on every
error path.** When the JSON parser hit `-EINVAL` (malformed token),
`-ERANGE` (more items than `max`), or `-ENOMEM` (allocation
failure) mid-array, it returned without (a) writing `*out_n` or (b)
freeing the items already malloc'd into `out[0..cnt-1]`. The three
call sites in `vmaf_dnn_sidecar_load` (`output_names`,
`feature_names` / `features`, `encoder_vocab`) iterated
`0..*out_n` for fallback cleanup — but `*out_n` was still the
zero-initialised value the caller had stamped, so the loop ran
zero times and the strings leaked. `vmaf_dnn_sidecar_free` doesn't
help either: it uses the same zero `n_*` field to bound its loops.
A user (or attacker) dropping a malformed sidecar next to a tiny
ONNX leaked ~7 string allocations per `vmaf_dnn_session_open`.

## Decision

Two coordinated changes in one PR:

1. **Delete the six dead fields** from `VmafModelSidecar` and the
   three consumer branches that read them. The consumer code
   already had a `mean = NULL; std = NULL;` initialisation that
   the branch could only override to NULL (since `has_norm` was
   always false) — collapsing the branches to literal `NULL, NULL`
   passes makes the dead behaviour explicit.

2. **Push allocation ownership down to the producer.**
   `extract_string_array` now (a) writes `*out_n = 0u` on entry so
   callers always observe a defined value, and (b) frees its own
   partial work via a `free_partial_string_array(out, cnt)` helper
   on every error return. The three call sites' fallback cleanup
   loops are retained as defence-in-depth (they now safely iterate
   zero times because the producer guarantees `out_n == 0` and
   `out[*]` slots NULL on error).

Regression coverage: two new tests in
`core/test/dnn/test_model_loader.c` construct malformed
`output_names` and `feature_order` arrays that exercise the
partial-allocation path and assert `n_* == 0` plus all slots NULL
post-parse. The tests were verified to fail without the producer fix
(stale pointers in `meta.output_names[0]`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Wire the JSON parser to actually populate `has_norm` / `norm_mean` / `norm_std` from sidecar keys | Restores the documented contract; future models could ship runtime scalers | Adds a code path nothing exercises — the canonical export bakes the affine into the ONNX graph. Two more producers (trainers) would need to learn to emit the keys. Pure scope creep. | Defer; if a real producer appears the parsing is trivially re-added |
| Keep the dead fields + branches, just fix the leak | Smaller diff | Leaves the producer / consumer contract violation in place; ADR-0114 already flagged it as a deferred cleanup | The audit's whole purpose is to close latent traps |
| Make `extract_string_array` skip freeing on error and force every caller to handle cleanup correctly | Producer stays lean | All three call sites already had a (broken) cleanup loop — they got the cleanup wrong because the API contract was non-obvious. Moving ownership to the producer makes the safe path the only path | Producer-side cleanup is the universally-safer C convention |
| Add a clang-tidy / cppcheck custom rule to flag "writes to out without setting out_n on error" | Catches the bug class globally | One-off pattern; no other repo functions have the same shape; tooling cost far exceeds the value | Single function, single fix |

## Consequences

- **Positive**: producer / consumer contract is consistent again
  (sidecar-driven luma normalisation is no longer an unstated
  half-feature); coverage on `dnn_api.c` lifts by ~3pp (dead-line
  removal closes one of the two ceilings called out in
  [ADR-0114](0114-coverage-gate-per-file-overrides.md)); malformed
  sidecar JSON no longer leaks string allocations on the
  session-open hot path (closes a bounded-but-real leak in an
  attacker-controlled input path).
- **Negative**: `VmafModelSidecar` ABI changes (six internal fields
  removed). The struct lives in `core/src/dnn/model_loader.h` — an
  *internal* header, not under `core/include/libvmaf/` — so no
  public-ABI consumer is affected. All internal callers are
  updated in the same PR.
- **Neutral / follow-ups**: [ADR-0114](0114-coverage-gate-per-file-overrides.md)'s
  per-file coverage override for `dnn_api.c` can be tightened once a
  coverage run confirms the new number; not changed here to keep the
  diff focused.

## References

- [ADR-0114](0114-coverage-gate-per-file-overrides.md) — Alternatives §2
  explicitly flagged "delete the dead `has_norm` branch in `dnn_api.c`
  (lines 141-144)" as a deferred follow-up. This ADR executes that.
- [ADR-0113](0113-ort-create-session-fallback-multi-ep-ci.md)
  References — also mentioned the `has_norm` removal as a separate
  cleanup.
- `core/src/dnn/AGENTS.md` — sidecar layout invariants.
- Source: `req` (paraphrased): user-directed deep audit of
  `core/src/dnn/` to surface a real correctness bug and bundle the
  fix as a single draft PR. The audit found exactly the latent
  contract violation ADR-0114 had already documented, plus a related
  partial-allocation leak in the same file.
