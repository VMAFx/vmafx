- **`vmaf-dev-llm commitmsg` can no longer hang indefinitely in Git.** Its
  staged-diff subprocess is shell-free, resolves the executable before spawn,
  disables external diff drivers, closes standard input, preserves failure
  diagnostics, and enforces a finite 30-second wall-clock timeout. The
  developer-helper package is also clean under the current Ruff profile, and
  its packaged prompts use real Markdown headings instead of lint disables.
  Model-card safety checks now read the real `core/src/dnn/op_allowlist.c`
  repository path instead of silently skipping a nonexistent legacy path.
