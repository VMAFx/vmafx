---
paths:
  - tools/vmaf-tune/src/vmaftune/classify.py
  - tools/vmaf-tune/tests/test_classify.py
invariant: Fail-by-name on missing vision backend; video frames never leave local machine (loopback/RFC1918 only).
---
<!-- markdownlint-disable MD024 -->
# Content-aware classifier invariants

- **Fail-by-name on missing vision backend (ADR-3374 supersedes ADR-0618).**
  `classify` with `genre=True` raises `VisionBackendUnavailable` when VLM
  backend is `None` or endpoint lacks vision support. Error message must name
  missing backend and config key (`--vlm-url` / `vlm`). Silent coercion to
  `"unknown"` is strictly prohibited.
- **Video frames never leave local machine (ADR-3374).**
  `OpenAICompatVlm` validates target host and refuses non-loopback /
  non-RFC1918 endpoints before initiating any HTTP connection (`allow_remote=False`
  default). Frames must never be dispatched to hosted cloud APIs.
- **Canonical genre vocabulary is immutable.**
  `GENRE_LABELS` is exactly `("movie", "tv", "anime", "adult", "sports", "unknown")`.
  Lawful adult content is a standard category; it is neither excluded nor
  coerced. Model output outside this vocabulary raises validation error.
- **Pure signal extraction is deterministic.**
  ITU-T P.910 SI/TI extraction via FFmpeg `siti` filter runs in-process or via
  mock runner without GPU or network dependencies.
- **Sidecar cache corruption recomputes with warning.**
  Invalid JSON in `<src>.classify.json` triggers a logged warning naming the
  sidecar file and transparently recomputes tags.
