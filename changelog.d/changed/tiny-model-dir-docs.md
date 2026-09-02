- **docs(ai)**: Document the implemented `VMAF_TINY_MODEL_DIR`
  tiny-model directory jail across the security, inference, and model
  registry docs. The docs now describe the fail-closed path semantics
  already enforced by `model_loader.c`: resolved ONNX paths must sit
  below the trusted directory, while sibling-prefix escapes, symlink
  escapes, missing jail directories, and non-directory jail values
  return `-EACCES`.
