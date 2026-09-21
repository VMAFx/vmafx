- Refactored the Python feature-extractor, asset, quality-runner, BD-rate, VMAFx CLI, and MCP
  regression tests into bounded helpers so every touched test function satisfies HISS without
  changing test discovery, fixtures, or any golden assertion, expected value, or tolerance.
