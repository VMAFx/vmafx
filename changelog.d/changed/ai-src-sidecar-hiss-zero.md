- **`ai/src` and `ai/sidecar` are at the HISS standard.** The recursive JSON
  walks (`_sanitize_nonfinite`, `normalise_manifest_value`,
  `_describe_path_value`) now share one iterative `aiutils.tree_utils.map_tree`;
  `op_allowlist` walks ONNX subgraphs with an explicit stack and reports
  findings in the same document order. `bisect_model_quality`, the three
  `vmaf-train` commands, `audit_learned_filter`, `export_to_onnx`, the parquet
  writers, `SGDEMATrainer.step` and the sidecar server loop are split into
  helpers that keep their names and signatures. No behaviour change; the HISS
  baseline loses the rows of these files.
