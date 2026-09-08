# Tensor I/O test lint cleanup (2026-09-08)

## Verified findings

The retained configured CPU cppcheck ledger reports 31 records for
`core/test/dnn/test_tensor_io.c`: 30 read-only fixture arrays that can be const,
and one informational branch-limit notice. They are not 31 runtime defects.
The actual CPU compile database and clang-tidy 22.1.8 additionally reproduce
three function-size findings and six casts deliberately outside the dtype or
resize enumeration. The unmodified test passes all 30 cases under ASan/UBSan.

The cleanup const-qualifies the 30 input arrays, splits the RGB fixture and
argument checks into helpers, and groups the existing driver calls. All 104
assertions retain exactly the same C tokens, including tolerances and expected
values. All 30 test cases retain their original order and execute once; helper
groups do not inflate the test counter. No production, scaffold, Python or
Netflix golden code changes.

## Intentional invalid-input probes

Five `(VmafTensorDType)99` casts and one `(VmafTinyResize)99` cast must remain
invalid to exercise `tensor_io.h`'s error contract. The actual conversion calls
and rejection assertions remain unchanged. Each cast has its own precisely
scoped `NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)` marker.
The existing C `NULL` compatibility bracket remains intact.

[ADR-1080](../adr/1080-ubsan-enum-invalid-value-log-opt.md) establishes that
changing invalid enum tests to valid inputs removes essential error coverage.
The touched-file exception rule permits a cited marker where refactoring would
break that invariant. No new ADR is needed: this preserves existing tests and
policy. Alternatives: a valid enum would remove the tested error path; hiding
the value with byte copying or volatile storage would obscure the analyzer's
input; a broad suppression would conceal unrelated mistakes. None is appropriate.

## Validation and limits

The existing Meson target compiles the real tensor I/O implementation directly;
DNN runtime support may be disabled without replacing it with a stub. In an
isolated CPU debug build with `-Db_sanitize=address,undefined`:

```bash
ninja -C build test/dnn/test_tensor_io
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  meson test -C build --print-errorlogs test_tensor_io
```

Before and after cleanup, all 30 cases pass with no sanitizer finding or skip.
Source-token and test-output receipts verify assertion and execution preservation.
The touched file has zero clang-tidy diagnostics under the configured profile
with all warnings treated as errors. Cppcheck uses all three actual target
commands, including the test runner and tensor implementation; it reports no
finding in the touched file. Its isolated target scope still reports the known
const-pointer finding in unchanged `core/test/test.c`, owned by a separate
cleanup, and a branch-limit informational record in the production tensor TU.

The scoped ratchet writer measures the actual test translation unit and lowers
its nine warnings to zero. The baseline total falls from 1435 to 1426 warnings;
uncited suppression debt remains 58. Every unmeasured entry and previous full-run
metadata is preserved. No baseline is hand-edited and no warning threshold changes.

Commands, logs, source/binary hashes and preservation receipts live under
`.workingdir2/evidence/tensor-io-test-cleanup-20260908`. Runtime uses the cached
CPU image `sha256:4e2b0298690d730e5ccbd6bd62d4ba9a03935808d4a88549a84e6bf7562f577d`,
CPU affinity 28–31, no network and no GPU. Whole-tree lint/test, Windows and
real ONNX inference acceptance are outside this bounded cleanup.
