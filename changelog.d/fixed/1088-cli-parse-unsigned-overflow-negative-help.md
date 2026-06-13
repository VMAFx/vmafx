**cli: parse_unsigned rejects negative and >UINT_MAX inputs; --help restored in production binary (ADR-1088)**

`parse_unsigned` in `cli_parse.c` / `cli_parse.cpp` silently accepted two
malformed inputs:

- Negative strings such as `--frame_cnt -1`: `strtoul` wraps them to `ULONG_MAX`
  without setting `errno`, so the value passed the `*end == '\0'` guard and was
  truncated to `UINT_MAX`, causing the frame loop to run for ~4 billion iterations.
- Values exceeding `UINT_MAX` on 64-bit hosts (e.g. `--frame_cnt 5000000000`):
  `strtoul` returned the 64-bit value without error; the silent `(unsigned)` cast
  wrapped it to an unrelated small number.

Both are now rejected with a clear `"Invalid argument … should be an integer in
[0, 2^32-1]"` message and `exit(1)`. Five regression tests added using the
existing fork/waitpid fixture.

Additionally, the `--help` flag was missing from `cli_parse.cpp` (the production
binary since ADR-0809) while present only in `cli_parse.c` (used by unit tests).
Running `vmaf --help` previously printed a confusing mandatory-argument error
rather than the help text. The flag is now wired in `cli_parse.cpp`.
