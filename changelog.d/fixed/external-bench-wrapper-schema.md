- **external-bench**: Validate wrapper JSON at the subprocess boundary
  and report malformed payloads as clear wrapper errors instead of
  letting aggregation fail later with `KeyError` / `TypeError`.
