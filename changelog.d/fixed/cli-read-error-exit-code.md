- **`vmaf` now exits 102 instead of 0 when an input stream fails to read.**
  The CLI's frame loop reported only a frame count, so a truncated or corrupt
  input was indistinguishable from a clean end of stream: the binary exited 0
  and wrote a full report over whatever prefix had arrived. When *both* inputs
  failed it did not even print a diagnostic, because the "both streams ended"
  test ran before the error test and a pair of `-1` return values satisfies it.
  Read failures now exit with the dedicated code `102`, print
  `problem while reading pictures`, and write no output file. A stream that
  legitimately ends earlier than its partner is unchanged — it keeps its
  `ended before` warning and exit 0. **Behaviour change**: a caller that
  silently consumed partial scores from truncated media will now see a non-zero
  exit. See [ADR-1262](docs/adr/1262-cli-input-read-error-exit-code.md).
