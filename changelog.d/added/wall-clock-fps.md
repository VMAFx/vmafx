- **Reports carry wall-clock throughput next to `fps` (ADR-3377).** The JSON
  report adds `"wall_fps"` after `"fps"` and the XML report adds a `wall_fps`
  attribute to `<fyi>`. `fps` stays as before: pictures per second of process
  CPU time summed over all threads, so a run with `--threads 8` reports a value
  several times below its real throughput. `wall_fps` is pictures per second of
  elapsed time from a monotonic clock
  ([output fields](docs/usage/cli.md)).
