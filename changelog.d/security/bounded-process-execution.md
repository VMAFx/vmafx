- Bound repository automation subprocesses to explicit executables, arguments,
  output memory, deadlines, and process-group cleanup instead of relying on
  static-analysis waivers at each call site; canonicalized the helper's Python
  package identity so direct scripts and type checks exercise the same API.
