### Fixed: `meson test --suite=fast` pre-push gate ran zero tests

`core/test/meson.build` declared ~40 `test()` entries but none carried a
`suite:` argument. Running `meson test -C build --suite=fast` — the
pre-push gate documented in `CLAUDE.md §3` — reported no tests and
silently exited 0, giving contributors a false green signal.

All test registrations now carry an explicit `suite:` list:

- `['fast']` — CPU-only unit tests finishing in <2 s (37 tests)
- `['fast', 'simd']` — arch-gated SIMD bit-exactness tests (7 tests)
- `['fast', 'gpu']` — GPU backend scaffold/contract smoke tests (13 tests,
  built only when the corresponding backend option is enabled)
- `['slow']` — `test_mcp_smoke` (60 s timeout)

Running `meson test -C build --suite=fast --list` now reports 37+ tests on
a CPU-only build instead of 0.

Identified by `.workingdir/audit-build-matrix-symbols-2026-05-16.md`
finding 5c and `.workingdir/audit-test-coverage-2026-05-16.md`.
