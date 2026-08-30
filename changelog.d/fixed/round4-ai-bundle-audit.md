- **Round-4 audit bug-fix bundle (AI training harness)**. Three verified
  data-integrity / durability defects from the adversarial audit of the
  admin-merged #1043–1062 batch (training-harness only; no libvmaf / CLI /
  model / Netflix-golden impact): (1) `online_trainer._export_checkpoint`
  incremented `_checkpoint_counter` and returned the path *before* the ONNX
  export ran, so a failed `export_onnx()` permanently burned a version number
  and returned a "ghost" path to a file that was never written; the counter now
  advances and the path is returned only after a successful, durable export
  (failure returns `None`, so the ACK carries `"checkpoint": null`). (2)
  `online_trainer.ingest` cleared `_pending` inside the lock but ran the
  gradient step outside it, so a `RuntimeError` in the PyTorch backward pass
  (CUDA OOM, shape mismatch) silently dropped the just-cleared samples; they
  are now snapshotted and restored to `_pending` on failure before the error is
  re-raised, so the batch is retried on the next ingest. (3)
  `extract_ugc_features` skips a clip whose integer aspect down-scale rounds a
  dimension below 2 (e.g. a 1px-wide source → `target_w == 0`), preventing both
  the ffmpeg `scale=0` error and a `ZeroDivisionError` in `_decode_to_yuv` on
  resume.
