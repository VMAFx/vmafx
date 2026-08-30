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
- Four gosec findings in the newly-ported Go code are resolved rather than
  suppressed wholesale: the `rune`→`byte` conversion in the plan-JSON ASCII
  escaper is reached only under a `r < utf8.RuneSelf` guard gosec cannot narrow
  (G115); the TPE sampler's seeded RNG is a reproducibility requirement, not a
  security decision, and its annotation is corrected from golangci-lint's
  `//nolint:gosec` (which standalone gosec ignores) to `#nosec G404`; and
  `VMAFTUNE_WORKDIR` is now `filepath.Clean`ed before use, with the remaining
  G703 taint justified inline as operator-supplied process configuration.
- `make lint-go` no longer exits 0 when gosec is absent — the same silent-skip
  defect fixed for the other gates.
