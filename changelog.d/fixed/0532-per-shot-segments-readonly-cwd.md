- `vmaf-tune tune-per-shot` no longer exits 1 when the working directory is
  read-only (e.g. a bind-mounted `/workspace` inside the dev container).
  The segment-directory derivation now prefers `<plan-out>.parent/segments`
  over `<output>.parent/segments` (the latter resolves relative to CWD when
  `--output` is at its default).  An `OSError` from `write_concat_listing`
  (e.g. a non-writable explicit `--segment-dir`) is now caught, a `WARN` is
  emitted to stderr, and the command exits 0 — the plan JSON is always the
  primary deliverable (ADR-0530).
