## CodeQL sweep: exact float compares in tests (2026-10-06)

`fix/codeql-test-float-compare`. Test-only: no rebase impact beyond the files named in
`changelog.d/fixed/codeql-test-float-bits-sweep.md`. Upstream-mirror tests keep their assertions;
only the comparison spelling moved to `core/test/float_bits.h` (ADR-1502).
