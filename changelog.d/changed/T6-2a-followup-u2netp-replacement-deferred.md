- **U-2-Net `u2netp` saliency replacement deferred (T6-2a-followup' /
  ADR-0265 / Research-0054)** — second blocker following ADR-0257
  (PR #328). [ADR-0257](docs/adr/0257-mobilesal-real-weights-deferred.md)
  recommended swapping the underlying model family from MobileSal to
  U-2-Net's `u2netp` (Apache-2.0, ~4.7 MB, pure RGB). Attempting that
  swap blocks on two independent findings — captured in
  [Research-0054](docs/research/0055-u2netp-saliency-replacement-survey.md):
  (1) upstream [`xuebinqin/U-2-Net`](https://github.com/xuebinqin/U-2-Net)
  carries a clean SPDX `Apache-2.0` `LICENSE`, but `u2netp.pth` is
  distributed only via Google Drive viewer URLs (no GitHub release,
  no pinnable raw URL — same blocker as MobileSal in ADR-0257); and
  (2) U-2-Net's `F.upsample(..., mode='bilinear')` lowers to the
  ONNX `Resize` op which is **not** on the fork's
  `core/src/dnn/op_allowlist.c`, and bilinear resampling has no
  exact decomposition into the existing allowlist primitives at
  dynamic stride. The smoke-only synthetic placeholder
  (`mobilesal_placeholder_v0`, `smoke: true`) remains shipped
  unchanged; the C-side `feature_mobilesal.c` extractor and its
  smoke test are not touched. Three follow-up rows filed in ADR-0265
  §"Neutral / follow-ups" (`T6-2a-widen-allowlist-resize`,
  `T6-2a-mirror-u2netp-via-release`, `T6-2a-train-saliency-student`).
  Aligns with the task-brief "don't fake it" directive — records the
  real reasons real weights aren't shipping rather than producing a
  graph that would look like real weights but couldn't be. Companion
  to [ADR-0218](docs/adr/0218-mobilesal-saliency-extractor.md) and
  [ADR-0257](docs/adr/0257-mobilesal-real-weights-deferred.md).
