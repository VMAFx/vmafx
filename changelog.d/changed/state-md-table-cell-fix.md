- `docs/state.md`: fixed 9 broken markdown table rows caused by `\|` characters
  (backslash-pipe) outside backtick code spans. Pipes inside table cells must be
  inside inline code spans to avoid being parsed as cell boundaries. Fixed rows:
  line 87 (missing Owner column added), lines 130/132/147/172/188/206/232/236/238/241/246/295
  (backslashes removed from pipes inside code spans; `CLOSED \| null` and
  `OPEN \| null` and `\|Δ\|` wrapped in backticks). All 9 affected tables now
  pass a backtick-aware cell-count validator.
