- **`vmaf-tune` predictor training accepts sharded corpus directories.**
  `vmaftune.predictor_train --corpus` can now point at a directory of
  JSONL shards (scanned recursively in sorted order), so existing
  `.workingdir2/corpus_run/` outputs feed real per-codec training
  without a manual concatenation step.
