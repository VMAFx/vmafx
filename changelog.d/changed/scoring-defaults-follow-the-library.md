- The MCP scoring tools (`vmaf_score`, `vmaf_score_encoded`,
  `describe_worst_frames`) use the library default model when `model` is
  omitted (`vmaf_v1.0.16_3d0h` in this release) instead of `vmaf_v0.6.1`;
  pass `model="version=vmaf_v0.6.1"` to reproduce earlier numbers. Their
  validation follows the generated schema: `threads` 0 (single-threaded) is
  accepted, a `feature` list with a non-string or empty entry is refused
  instead of filtered, and errors name the argument (`invalid tiny_crf 64:
  must be <= 63`).
- The scoring server returns lossless scores (`precision` `max`) by default,
  so its scores equal the CLI's and the C API's bit for bit; a request may
  still ask for another precision. A request body with a field the contract
  does not know is refused with 400.
- `vmaf --help` lists every option with its values and default, generated
  from the API definition.
