# Research-0513: tiny-model loader and the rank-2 / external-data gap

## Question

Why did `vmaf --tiny-model fr_regressor_v1.onnx` (and the v2 + tiny_v4
checkpoints) fail with `-95` when ONNX Runtime, the trainer, and the
Python harness all happily loaded the same files?

## Investigation summary

1. **Reproduce.** Run the three shipped checkpoints through
   `vmaf --tiny-model … --tiny-device cpu` against the Netflix
   `src01_hrc0[01]_576x324.yuv` fixture. All three exited with
   `"problem loading tiny model …: -95"`. `errno 95` is `ENOTSUP` on
   glibc.
2. **Search for the error path.** `grep` for the message located
   `core/tools/vmaf.c::configure_tiny_model` printing whatever
   `vmaf_use_tiny_model` returned. Tracing that into
   `dnn_attach_api.c` showed three places that could surface
   `ENOTSUP`: the ONNX Runtime CreateSession, the input-shape probe,
   and `vmaf_ctx_dnn_attach`.
3. **Bracket with an ORT probe.** A standalone 40-line C program
   linked against the in-container `libonnxruntime.so.1` opened all
   three models successfully, including the external-data v1/v2
   files, and printed input rank=2 with shapes
   `[-1, 6]` (`features`) and `[-1, 14]` (`codec`, v2 only). This
   proved (a) external-data resolution works with no special
   configuration when `CreateSession(env, abs_path, …)` is given the
   absolute model path and (b) the rejection lived in libvmaf, not
   ORT.
4. **Identify the gate.** Reading `libvmaf.c::vmaf_ctx_dnn_attach`
   surfaced `if (in_rank != 4) return -ENOTSUP;` (line 737 pre-fix).
   Pre-fix, the bridge only accepted NCHW image models.
5. **Determine the trainer's contract.** The three sidecars carry
   the canonical-6 feature list under either `feature_order`
   (v1, v2) or `features` (vmaf_tiny_v4), plus a StandardScaler
   under `feature_mean`/`feature_std` or `input_mean`/`input_std`.
   `ai/scripts/train_fr_regressor_v2.py::_row_to_features` showed
   the codec block layout (N-encoder one-hot + preset_norm + crf_norm,
   width 14 for vocab v2). The "unknown" one-hot lives at the
   third-from-last index in that layout.
6. **Decide the fix surface.** Three options surveyed (see ADR-0517
   §Alternatives). The chosen path branches the existing attach +
   run path on rank: rank-2 materialises the feature vector from
   the live feature collector each frame, applies the sidecar's
   scaler if present, and dispatches via `vmaf_ort_run` when a
   second input is present (v2 codec block, pre-seeded to "unknown").

## Findings of independent interest

- **ORT external-data is implicit.** `CreateSession` with an
  absolute path automatically resolves sibling `.onnx.data`. The ORT
  C-API has the explicit `AddExternalInitializersFromFilesInMemory`
  surface, but it is only needed when the caller wants to feed
  external initializers from a non-filesystem source — not for the
  on-disk sibling-file case the fork uses.
- **Two sidecar conventions in tree.** Both
  `feature_order`/`feature_mean`/`feature_std` (FR regressors) and
  `features`/`input_mean`/`input_std` (`vmaf_tiny_v*`) are
  in-tree. The parser supports both so the loader does not
  arbitrarily pick a "winner" trainer convention.
- **motion2 retroactive write timing.** Feature-vector inference
  runs in `read_pictures_post_extractor`, AFTER the extractor loop
  completes for the current frame. `motion2` writes to frame N once
  frame N+1's SAD is computed (ADR-0152), so the first frame's
  inference sees `motion2 == 0.0` from the feature collector. This
  is observable but bounded; the alternative (deferring tiny
  inference by one frame) was not pursued in this PR.
- **`fr_regressor_v2`'s codec one-hot is best-effort today.** The
  codec block defaults to the "unknown" encoder slot at attach
  time. A future PR can wire `--tiny-codec` / `--tiny-preset` /
  `--tiny-crf` CLI flags through to `extra_in_buf` so codec-aware
  models get the correct context; until then, scores from v2 will
  diverge from the Python reference. The load + run gate is green
  regardless.

## Decision delta vs status quo

Before: only `NCHW` `[1, 1, H, W]` ONNX models worked; the three
production FR regressors were unreachable from the CLI. After: the
three FR regressors load and run; the rejection message for
unsupported ranks names the actual rank instead of just exiting
with a raw errno.

## Related artifacts

- ADR-0517 (this fix).
- Trainers: `ai/scripts/train_fr_regressor.py`,
  `ai/scripts/train_fr_regressor_v2.py`.
- Sidecars: `model/tiny/fr_regressor_v1.json`,
  `model/tiny/fr_regressor_v2.json`,
  `model/tiny/vmaf_tiny_v4.json`.
- e2e diagnostic chain that surfaced the bug:
  `.workingdir/bbb_reports/E2E_TEST_MATRIX_v9.md` items 2d / 2e / 2f.
