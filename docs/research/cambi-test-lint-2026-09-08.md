# CAMBI native test cleanup

The exact `fa792ba4d` CPU profile reports 12 clang-tidy diagnostics in
`core/test/test_cambi.c`: ten large test bodies, the oversized test dispatcher,
and its deliberate inclusion of the private implementation. Cppcheck reports
24 read-only pointer/array findings. The existing 23 test cases pass before
cleanup.

Large assertion groups are now named helpers for individual bit depths,
resolution ranges, mask results and contrast arrays. Four small dispatcher
helpers preserve the original 23 case registrations and their order. Read-only
fixture arrays and views are const. Helpers that receive `VmafPicture` values
borrow their backing buffers: they do not create or release picture references.
The original owners still release each picture once. The called decimation
and mask helpers write output pixels, not the borrowed picture metadata.

A retained token-level comparison checks all 144 original `mu_assert` calls
(including their expressions and messages), all 30 array initializer token
sequences, and the 23 registrations in order. They are unchanged. Negative
cases, scalar/AVX2 bit-exact comparisons, expected scores and runtime CPU gates
are preserved. No production source, public API, golden assertion or FFmpeg
integration changes.

The one new `bugprone-suspicious-include` exception is narrow and follows
ADR-0141: these tests call private static helpers defined in `feature/cambi.c`.
Replacing that include with the public header would remove the tested helpers;
exporting them solely for testing would broaden the production interface. The
exception documents the existing deliberate test boundary. No size or numeric
comparison check is suppressed.

The existing Meson command is `meson test -C build test_cambi`. The same 23
cases pass after cleanup. Actual clang-tidy and cppcheck report no diagnostic
in the touched test source; the scoped baseline writer reduces only its CPU
warning allowance to zero. Reduced analysis context can still identify unused
helpers in included, untouched implementation/header files, so this receipt
is not a full-tree lint pass. CPU checks do not establish GPU, Windows, Darwin
or Netflix Python golden acceptance.

No new ADR is needed for this test refactor. Keep the private-source include,
case order and existing assertion values during rebases. Exact commands,
analyzer logs, before/after receipts and the comparison script live under
`.workingdir2/evidence/2026-09-08-test-cambi-lint/`.
