<!-- markdownlint-disable MD013 MD060 -->
# ADR-0639: Scaffold-audit P1 — backend precheck, HIP picture, mobilesal bpc, DNN multi-output

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `python`, `hip`, `ai`, `dnn`, `docs`, `vmaf-tune`

## Context

The scaffold audit conducted on 2026-05-19 (`docs/research/scaffold-audit-2026-05-19.md`)
identified four P1 ("feature accepted but does not plumb through") findings:

**P1-1 — `vmaf-tune compare` / `tune-per-shot` skip `select_backend()` precheck.**
Three subcommand handlers (`_run_compare`, `_run_compare_crf_sweep` via `_run_compare`,
and `_run_tune_per_shot` / `_build_per_shot_bisect_predicate`) assigned
`score_backend = None if args.score_backend == "auto" else args.score_backend` and
passed the raw string directly to `bisect_target_vmaf()`.  The `ladder` and `corpus`
subcommands already called `select_backend()` first and raised `BackendUnavailableError`
(exit 2) when the requested backend was unavailable.  The three affected paths surfaced
the failure mid-bisect as a cryptic vmaf binary error, not a clean exit-2 message.
Tracked as T-PYTHON-COMPARE-NO-BACKEND-PRECHECK in `docs/state.md` (ADR-0556).

**P1-2 — `core/src/hip/picture_hip.c` is a full stub.**
`vmaf_hip_picture_alloc` and `vmaf_hip_picture_free` returned `-ENOSYS`
unconditionally, blocking zero-copy HIP picture upload for all 9 HIP extractors that
called these functions.  Tracked as part of the T7-10 follow-up backlog.

**P1-3 — `feature_mobilesal.c` rejects `bpc != 8` with an unhelpful error.**
The error message `"mobilesal: bpc=%u not supported yet (8-bit only in this build)"`
did not name the extractor prominently enough to diagnose failures when multiple
`--feature` flags were passed.  The `docs/ai/models/mobilesal.md` §Known limitations
section also lacked a verbatim example of the error and a clear workaround.

**P1-4 — DNN multi-output returns `-ENOTSUP` with no public documentation.**
`vmaf_ctx_dnn_run_frame_nchw` and `vmaf_ctx_dnn_run_frame_feature_vector` returned
`-ENOTSUP` when the ONNX session produced `out_n > 1` scalars.  The limitation was
undocumented in the public C API header and absent from `docs/api/dnn.md`.

## Decision

**P1-1**: Insert `select_backend()` pre-checks at the top of `_run_compare` (before
the `no_bisect` dispatch) and at the top of `_run_tune_per_shot`.  On
`BackendUnavailableError`, print an actionable message to stderr and return 2.  The
comment block in `_run_tune_per_shot` that deferred the fix (citing ADR-0509) is
removed; the defer rationale was superseded by the availability of the pre-check
pattern established in `_run_ladder` and `_run_corpus`.  The `_build_per_shot_bisect_predicate`
and `_run_compare_crf_sweep` local `score_backend` assignments are annotated with
ADR-0639 citations.

**P1-2**: Implement real `hipMalloc`-backed `vmaf_hip_picture_alloc` and `hipFree`-backed
`vmaf_hip_picture_free` under `#ifdef HAVE_HIPCC`.  The non-HIP build retains `-ENOSYS`
in the `#else` branch so non-ROCm builds remain unaffected.  A pitched-allocation
follow-up (T7-10c: `hipMallocPitch` for per-plane alignment) is documented in the
file header but deferred — the current API signature passes a flat `size` byte count
(not `width_bytes + height` per plane), so changing it would touch all 9 extractor
sites.

**P1-3**: Improve the `mobilesal_init()` error message to explicitly name
`feature_mobilesal` as the blocker, state the 8-bit constraint, and give the
workaround (`--bitdepth 8` or omit `--feature mobilesal`).  Update
`docs/ai/models/mobilesal.md` §Known limitations with a verbatim copy of the new
message, the workaround, and an ADR-0639 citation.  Option (b) — implement 10-bit
support by downscaling/clipping to 8-bit internally — is rejected because it would
require retraining the saliency model on 10-bit-clipped input to avoid a distribution
mismatch that would silently degrade saliency quality.

**P1-4**: Add inline code comments at both `-ENOTSUP` sites in `libvmaf.c` citing
ADR-0639 and the T-DNN-MULTI-OUTPUT follow-up.  Update `docs/api/dnn.md` §Known
limitations with a clear description of the single-output constraint, an explanation
of why it exists (the `vmaf_feature_collector_append` API accepts one `double` per
frame per feature name), the workaround via standalone `vmaf_dnn_session_run()`, and
a pointer to the follow-up.  Open T-DNN-MULTI-OUTPUT in `docs/state.md` to track the
`vmaf_feature_collector_append_many`-backed multi-output follow-up.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| P1-1: Keep deferred (ADR-0509 rationale) | No risk of regression in compare/per-shot backend handling | Cryptic errors persist; inconsistency vs corpus/ladder remains | The defer rationale cited the ADR-0509 "do not regress auto-behaviour" rule, but `select_backend(prefer="auto")` already returns a valid backend and maps to `None` correctly — the regression concern was unfounded. |
| P1-2: Pitched allocation via `hipMallocPitch` | Better memory bandwidth alignment on tiled GPU memory | Requires changing `vmaf_hip_picture_alloc` signature from flat `size` to `(width_bytes, height)` — touches all 9 extractor call sites | Deferred to T7-10c; signature change is a larger blast-radius refactor than warranted for this PR. |
| P1-3: Implement 10-bit support (downscale + clip) | Eliminates the `bpc != 8` barrier for HDR content | Requires retraining on 10-bit-clipped input; silently degrades saliency quality without retraining; more complex than a doc fix | Rejected — per task spec, option (a) (early-reject with actionable message) is the correct choice when retraining is needed for option (b). |
| P1-4: Remove -ENOTSUP branch entirely | Cleaner code | Makes multi-output models appear to succeed but record no score — silent data loss is worse | Retained as an explicit guard with clear documentation; the TODO is tracked as T-DNN-MULTI-OUTPUT. |

## Consequences

- **Positive**: (P1-1) `vmaf-tune compare --score-backend cuda` on a CPU-only binary
  now exits 2 immediately with a `BackendUnavailableError` message instead of failing
  mid-bisect.  (P1-2) HIP picture allocation no longer returns `-ENOSYS`; extractors
  using `vmaf_hip_picture_alloc` can perform actual device memory allocation.  (P1-3)
  Operators scoring HDR content with `--feature mobilesal` receive an actionable error
  at init time rather than an opaque `-ENOTSUP`.  (P1-4) The multi-output limitation
  is visible in the public API docs.
- **Negative**: None material.  The comment removal from `_run_tune_per_shot` loses a
  small amount of historical context (the ADR-0509 defer rationale) — this is acceptable
  because the rationale is now superseded.
- **Neutral / follow-ups**: T7-10c (pitched HIP picture allocation with API signature
  change); T-DNN-MULTI-OUTPUT (multi-output ONNX support in the attached-mode path).

## References

- `docs/research/scaffold-audit-2026-05-19.md` — source audit; P1-1 through P1-4.
- [ADR-0556](0556-python-mcp-ai-audit-2026-05-18.md) — prior audit that first
  documented T-PYTHON-COMPARE-NO-BACKEND-PRECHECK.
- [ADR-0511](0511-vmaftune-ladder-backend-precheck.md) — `ladder` `select_backend()`
  pattern this PR replicates.
- [ADR-0509](0509-vmaftune-compare-container-source-autoprobe.md) — the ADR whose
  defer rationale was removed from `_run_tune_per_shot`.
- [ADR-0212](0212-hip-backend-scaffold.md) — original HIP scaffold that stubbed
  `picture_hip.c`.
- [ADR-0218](0218-mobilesal-saliency-extractor.md) — mobilesal extractor design.
- [ADR-0040](0040-dnn-session-multi-input-api.md) — multi-input/output named-binding
  API for standalone sessions (not affected by this ADR).
- `req` — "Implement the 4 P1 findings from scaffold-audit-2026-05-19.md in one draft PR."
