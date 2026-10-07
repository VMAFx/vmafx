- **Model hashes are the same on Windows.** The repository checked the model
  JSON files out with CRLF line endings on Windows, so a Windows build's
  built-in models and any model file loaded there reported a different
  `vmafx_model_hash()` than on Linux and macOS (and than `sha256sum` of the
  published file). The model JSON files now check out with LF on every
  platform.
