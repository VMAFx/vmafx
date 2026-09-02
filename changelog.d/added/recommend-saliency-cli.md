- **`vmaf-tune recommend-saliency` CLI subcommand
  ([ADR-0287](../docs/adr/0287-vmaf-tune-saliency-aware-encoding.md)).**
  Surfaces the existing saliency-aware encode pipeline (Bucket #2)
  as a runnable subcommand. Builds an `EncodeRequest` from the
  flag set, delegates to
  [`saliency.saliency_aware_encode`](../tools/vmaf-tune/src/vmaftune/saliency.py),
  emits a JSON summary (encoder + version + crf + size + exit
  status). Distinct from `recommend` (master's coarse-to-fine
  target-VMAF search) — saliency is a single-encode workflow
  that biases bits toward salient regions. Falls back to a plain
  encode when onnxruntime / the model is unavailable so the
  caller always gets a result.
