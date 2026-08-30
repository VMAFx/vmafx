- `pkg/benchmark/testdata/corpus.jsonl` is tracked again. `.gitignore` carried a
  bare `corpus.jsonl` pattern — meant for vmaf-tune Phase A scratch output — that
  matches at any depth, so the test fixture was silently excluded from the commit
  that added it and the package's tests failed on a missing file. A
  `!**/testdata/corpus.jsonl` negation keeps the scratch-output intent while
  exempting committed fixtures.
- The benchmark CSV golden fixtures keep their CRLF line endings. Python's `csv`
  module writes CRLF, and the Go renderer matches it, but `* text=auto` in
  `.gitattributes` normalised the CRs out on commit. The goldens therefore stopped
  matching the renderer — invisible in the worktree that authored them, which
  still held the pre-normalisation bytes, and red on any fresh checkout.
  `pkg/benchmark/testdata/*.csv` is now marked `-text`.
- Documented (not yet fixed, see ADR-1125) that `pkg/libvmaf`'s cgo `LDFLAGS`
  points at `-L${SRCDIR}/../../core/build-cpu/src`, and that when that directory
  does not exist the linker silently falls through to a distro-installed
  `libvmaf` rather than failing. On a host with the upstream package that is
  libvmaf 3.2.0 with zero `vmaf_dnn_*` symbols, so a Go binary can link against a
  library that is not this fork.
