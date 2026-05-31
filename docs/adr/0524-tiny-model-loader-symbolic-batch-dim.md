<!-- markdownlint-disable MD060 -->
# ADR-0524: Tiny-model loader accepts symbolic batch dim

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: `ai`, `dnn`, `loader`, `bug-fix`

## Context

ADR-0518 widened the tiny-model loader to accept rank-2 feature-vector
inputs alongside the legacy rank-4 NCHW image inputs. The next class of
shipped tiny model — the NR metric checkpoint
`model/tiny/nr_metric_v1.onnx` — exposes a different blocker: its input
graph declares the shape

```text
frame: [batch, 1, 224, 224]
```

where the first dimension is the symbolic ONNX `dim_param` token
`'batch'`. ONNX Runtime exposes symbolic dims through the C API
(`GetDimensions`) as `-1`. The libvmaf-side bridge
(`vmaf_ctx_dnn_attach` → `dnn_attach_nchw` in `core/src/libvmaf.c`)
rejected anything other than `in_shape[0] == 1`, so every model exported
with a Pythonic dynamic batch failed at attach time with `-ENOTSUP`
(errno 95). The CLI agent (PR #1280) flagged this as the next blocker
after wiring `--no-reference` through to the scoring path: every shipped
NR model uses this idiom.

The fork's per-frame inference loop only ever feeds one frame at a time
(`vmaf_ctx_dnn_run_frame_nchw` builds `int64_t shape[4] = {1, …}` for
the ORT Run call) — there is no batched-inference scheduler. A symbolic
batch on the *declared* model input is therefore safe to fold to 1 at
attach time: the runtime contract already pins batch=1 on every call.

The same reasoning applies to:

- The rank-2 feature-vector path (`dnn_attach_feature_vector`), where
  `[batch, F]` with a symbolic `batch` is the common trainer-side
  pattern (PyTorch / ONNX exporters default to dynamic batch).
- The optional second input on rank-2 models (the
  `[batch, codec_block_width]` `codec` input on `fr_regressor_v2`),
  which was also blanket-rejected on `extra_shape[0] != 1`.

Spatial dims (H/W) cannot be folded the same way — the scratch buffer
size depends on them, so the loader still requires a fixed positive
value and surfaces a sharper diagnostic when a "dynamic-resolution"
export reaches the gate.

## Decision

`vmaf_ctx_dnn_attach`'s helpers accept `in_shape[batch] ∈ {1, -1}` for
both the rank-4 NCHW and rank-2 feature-vector paths, and for the
optional rank-2 second input. Fixed batch > 1 stays rejected (no
multi-sample inference loop exists today). Symbolic H/W stays rejected,
but with a human-readable diagnostic distinguishing
"dynamic-resolution" from the unrelated "C != 1" failure.

Concrete changes:

- `dnn_attach_nchw` (libvmaf.c): accept `in_shape[0] ∈ {1, -1}`; split
  the "channels != 1" reject out of the combined gate so each failure
  surface gets its own diagnostic; clarify the H/W reject to mention
  symbolic dims and recommend re-export with a fixed resolution.
- `dnn_attach_feature_vector` (libvmaf.c): add the same batch policy
  before the feature-width check.
- Same policy applied to the optional second-input shape probe.
- Test fixtures (`core/test/dnn/test_vmaf_use_tiny_model.c`): add a
  regression case that synthesises a minimal rank-4 ONNX with
  `dim_param='batch'` on the first axis and asserts it attaches
  successfully via the public `vmaf_use_tiny_model` API.
- `core/test/dnn/test_cli.sh` step 5b: switch the `--no-reference`
  smoke from `dists_sq.onnx` (which load-fails for unrelated reasons)
  to `nr_metric_v1.onnx`, which now loads + scores end-to-end. The
  pre-existing comment "the shipped NR-marked models all use a
  symbolic batch dim that the loader rejects" is the regression marker
  that this ADR closes.

ORT-side semantics: `vmaf_ort_input_shape{,_at}` is a thin pass-through
over the ORT C API; it surfaces `-1` for symbolic dims verbatim. No
ORT-side change is needed — the policy lives in the libvmaf bridge.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fold symbolic batch at the ORT-shape probe layer (`vmaf_ort_input_shape`) so the bridge never sees `-1` | One place to enforce the rule; callers stay simple | Hides ORT's actual contract from any future caller that legitimately needs to see the symbolic dim (e.g. multi-batch tooling) | The bridge is the right policy boundary; the ORT wrapper stays a faithful pass-through |
| Hard-code batch=1 reject and tell users to re-export their NR models | Smallest patch | All three NR variants would need re-export; this is the same UX trap ADR-0518 closed for rank-2 | Punts the problem onto every model author |
| Add a `--tiny-batch` CLI flag and a real batched-inference loop | Enables future throughput optimisation | Out of scope; no in-tree consumer needs >1; the scoring loop is per-frame by design | Defer to a dedicated ADR if/when batched inference becomes a goal |
| Accept symbolic H/W too by deferring scratch alloc to the first frame | Lets dynamic-resolution exports load | Buffer realloc per frame defeats the constant-shape contract `vmaf_ort_run` enforces (`shape[d] > 0`); also breaks the W/H drift check in `run_frame_nchw` | Symbolic H/W stays rejected; the diagnostic just gets sharper |

## Consequences

- **Positive**:
  - `model/tiny/nr_metric_v1.onnx` loads and produces scores via
    `vmaf --no-reference --tiny-model …` — the gate the CLI agent's
    PR #1280 surfaced.
  - Every future tiny model exported with the PyTorch/ONNX default
    dynamic batch loads without re-export.
  - The diagnostics now distinguish "fixed batch > 1", "C != 1", and
    "symbolic H/W" instead of collapsing all three into one
    `-ENOTSUP`, which the e2e test agent burned investigation budget
    on during ADR-0518 work.
- **Negative**:
  - The loader silently folds symbolic batch to 1. A model author who
    really wanted batched inference will see scores compute but on
    one sample at a time. The acceptable trade is documented in
    `docs/ai/inference.md`.
- **Neutral / follow-ups**:
  - If a future use case needs honest batched inference, add a
    `vmaf_dnn_run_batch` C-API plus a CLI knob — this ADR explicitly
    declines to scaffold that.
  - Symbolic H/W remains a gap; a "dynamic-resolution" tiny model
    would need a per-frame realloc path. No in-tree model uses that
    pattern today.

## References

- `req`: user briefing in agent dispatch "Fix `vmaf_ctx_dnn_attach`
  in VMAFx/vmafx which rejects ONNX models with a symbolic batch
  dimension — surfaced by the `--no-reference` wiring agent
  (PR #1280) as the next blocker." (2026-05-18).
- ADR-0518 — predecessor; accepted rank-2 + external-data ONNX.
- ADR-0520 — `--no-reference` wiring that surfaced this blocker.
- ADR-0042 — tiny-AI docs-required-per-PR rule (drives the
  `docs/ai/inference.md` update).
- ADR-0108 — six-deliverable rule (research digest under
  `docs/research/`, decision matrix above, AGENTS.md invariant,
  reproducer, changelog fragment, rebase-notes entry).
- ONNX Runtime C API: `GetDimensions` returns `-1` for symbolic
  dims (see `onnxruntime/core/session/onnxruntime_c_api.cc`).
- PyTorch ONNX exporter dynamic-batch default:
  `torch.onnx.export(..., dynamic_axes={'frame': {0: 'batch'}})` is
  the idiom every shipped NR checkpoint uses.
