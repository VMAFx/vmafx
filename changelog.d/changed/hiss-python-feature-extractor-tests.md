- Refactored the Python feature-extractor, asset, executor, quality-runner, local-explanation,
  model-training, BD-rate, VMAFx CLI, reader, bootstrap-model, MCP, cross-backend parity, and
  Git-isolation regression harnesses into bounded helpers so every touched function satisfies HISS
  without changing test discovery, fixtures, CLI behavior, or any golden assertion, expected value,
  or tolerance.
