- **`tools/vmaf-tune fast` Phase A.5 — proxy-based recommend scaffold
  (ADR-0276 Proposed, Research-0060).** New opt-in CLI subcommand that
  combines a tiny-AI VMAF proxy (`fr_regressor_v2`, ADR-0272) with
  Optuna's TPE Bayesian sampler and a GPU-accelerated VMAF verify
  step to collapse the recommendation use case from the Phase A grid's
  hours-long wall-time to seconds-to-minutes. The slow Phase A grid
  path stays canonical as the ground-truth corpus generator
  (ADR-0237 contract); fast-path is opt-in via `pip install
  vmaf-tune[fast]`. This PR ships the scaffold only — Optuna search
  loop, smoke-mode synthetic predictor, CLI subcommand, production-
  shape entry point, AGENTS.md invariants. The real encode + ONNX
  inference + GPU verify wiring is a follow-up PR gated on Phase A
  corpus existence and `fr_regressor_v2` weights training (PR #347).
  Run `vmaf-tune fast --smoke --target-vmaf 92` to exercise the
  pipeline end-to-end without ffmpeg, ONNX Runtime, or a GPU.
