- **`cmd/vmafx-mcp`: `floatArg` ignored Go `int` values.** Only `float64` and
  `json.Number` were handled, so an integral argument arriving from an in-process
  caller (a test, or a dispatch path that does not round-trip through JSON)
  silently fell back to the parameter's default and skipped its bounds check —
  e.g. `target_vmaf: 101` was accepted and became 90. `intArg` had always handled
  the mirror-image case. No JSON-RPC client was affected, because JSON decoding
  produces `float64`.
