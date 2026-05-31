- **`semgrep-local` pre-commit hook silently failing with exit 2.**
  semgrep-core (the OCaml engine behind `semgrep scan`) uses Eio, which
  spawns one io_uring submission queue per worker. On
  `pre-commit run --all-files` pre-commit batches the matched targets into
  chunks and runs the chunks in parallel; the per-process io_uring memlock
  charge then exceeds `ulimit -l` (8 MB default on most distros) and
  semgrep-core dies with `Unix_error: Cannot allocate memory
  io_uring_queue_init`. The Python wrapper surfaces this as exit 2 —
  silently, because the hook's `--quiet` flag swallows the stderr trace.
  The bug blocked the ISO header-guards PR (#481) and contributed to the
  accidental nuking of the doxygen PR (#457) when a commit got blocked but
  the push went ahead. Fixed by adding `require_serial: true` (so
  pre-commit runs a single semgrep process at a time) and `--jobs 1` (so
  that one process scans serially). Cost on the staged-files commit path
  is negligible (≈2 s → ≈4 s); the all-files path goes from "always fails"
  to "works".
