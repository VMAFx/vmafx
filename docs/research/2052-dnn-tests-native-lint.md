# Research-2052: DNN test const inputs and fixture I/O errors

## Findings and fix

A fresh CPU build at parent `fb79390160fdfc3f8316dd5ab6cae4c05de2b58d`
reproduces nine `constVariable` findings in `test_ort_internals.c` and seven
in `test_dnn_session_api.c`. These are arrays passed through existing const
input/shape parameters. The corresponding production declarations already
accept const, so only the sixteen local declarations change.

Strict clang-tidy finds no issue in ORT internals, but the session test has
an oversized driver (70 branches, limit 15) and two stream diagnostics in
`copy_file`. The helper retries `fread` after a positive short read and never
checks the read-error indicator. Calling the actual helper with a directory
source reproduces `copy_result=0` despite the read error. It now stops after
a short read and returns `-1` when `ferror` is set. Existing callers retain
their existing handling of copy failures. Successful copying and production
DNN behavior are unchanged.

One POSIX regression uses the same actual helper, a directory source, and an
atomically created output file under the runtime-resolved `P_tmpdir` location.
It closes/unlinks its resources before checking the result. The seven groups
of original session cases and one group for this new case propagate failure
messages without adding helper groups to the test count. All existing case
names and ordering remain intact, including Windows guards.

No alternatives: const implements the existing input contract; driver grouping
implements the current branch limit; a read error cannot be a successful copy.
These are implementation fixes under existing rules, with no new ADR or
suppression. The existing ADR-1138 C `NULL` brackets are unchanged.

## Preservation and failure controls

Every one of the 83 original test functions is token-identical except for the
sixteen inserted `const` tokens. This preserves all 208 original assertion
expressions/messages, fixtures, input literals and 298 `vmaf_*` call sites.
The ORT table retains 48 entries; the session runner retains its original 35
registrations in order and adds one POSIX copy-error case. Meson registration
is byte-identical to the parent.

The complete non-LTO ORT test object has byte-identical `.text` (10,644 bytes)
and every `.rodata`/`.data` section before and after. Disabling LTO in this
control keeps the complete test body visible to the compiler even when an
external availability function could otherwise be folded. The session object
is intentionally not claimed identical because its copying helper, driver
and regression change.

The new regression fails with exit one and the expected assertion when only
`copy_file` is restored to the actual parent body in a disposable copy of the
current test source. Seven valid source sizes (0, 1, 4095, 4096, 4097, 8192,
8193 bytes) produce byte-identical copies before and after, including empty,
short, exact-buffer and multiple-buffer boundaries. Independent directory
copy controls return zero before and minus one after.

Link-time public-API failure controls cover one case in each of the seven
original driver groups. Original/current executables produce the same exit
one, stdout, assertion failure and reported case order. The eighth group's
new regression is covered by restoring the old copier. Raw stderr is retained;
random temporary model paths and ORT diagnostics are not required to be
byte-identical. Only disposable test/wrapper objects disable LTO for reliable
link-time interception; production objects are unchanged.

## Native validation and limits

Fresh GCC 15.2 CPU release builds run both existing binaries with DNN disabled
and with the image's pkg-config ONNX Runtime 1.29.0 enabled. Before: 48 + 35
registered cases. After: 48 + 36 on POSIX. Both builds pass, and the enabled
run actually opens the smoke models through ORT. Disabled-DNN guards remain
unchanged and do not count as inference coverage. The private container has
no GPU devices/network and uses CPUs 28–29; no runtime dependency is installed.

Both whole touched files pass clang-tidy 22.1.8 with all warnings treated as
errors. Exhaustive Cppcheck 2.21.1 with the official POSIX model exits zero
for the 139-command focused database, retaining every matching variant and
`test.c` context. The actual scoped writer leaves ORT's zero debt unchanged
and tightens the recorded session-test allowance 2→0; total CPU warning debt
changes 1270→1268, with 44 uncited exceptions unchanged. All unmeasured entries
and full-report metadata are preserved.

This component does not claim fresh whole-tree `make lint`/`make test`, GPU,
Windows/Darwin, sanitizer or Netflix golden acceptance. Production sources,
headers, ONNX model bytes and golden assertions are untouched.

## Reproduce and retain

```sh
meson test -C build --print-errorlogs test_ort_internals test_dnn_session_api
python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
  --only core/test/dnn/test_ort_internals.c \
  --only core/test/dnn/test_dnn_session_api.c
```

The canonical receipt is
`.workingdir2/evidence/dnn-tests-native-lint-20260908/`: source hashes, exact
container/build/analyzer commands, positive and negative controls, object
section hashes, raw logs, generated baseline and normal hooks. Binaries and
build objects stay in the matching declared cache. Standalone publication is
not requested; root-owned integration remains pending.

## Follow-up: buffered destination close failure

At `2d135979175a5c9beb2f585232fe91448e16e032`, the copier still discards
`fclose(fdst)`. A one-byte regular source normally copies one byte and returns
zero. With `RLIMIT_FSIZE=0` and `SIGXFSZ` ignored only in a disposable child,
`fwrite` buffers that byte and the final flush fails: the actual old helper
returns zero while the destination remains empty. The fix checks destination
`fclose` after closing the source, and returns minus one on failure. Both
streams are always closed; successful copying and production code are unchanged.

The new POSIX case creates private regular files, checks an unrestricted copy,
and forks before any ORT initialization. Only the child changes its resource
limit and signal disposition. Exit two means child setup failure; exit three
means the copier incorrectly accepted the failure; zero means correct rejection.
The parent reaps the child and removes both files before assertions, reporting
setup failure before checking empty output. No device files are involved.

All 36 previously registered session cases are token-identical, including the
read-error regression; the 35 original cases retain their relative order and
the read-error case stays last. The new case is a prefix, so POSIX session
counts increase 36→37, and internals remain 48: 85 total. Whole test-output
counts are intentionally not claimed identical. Original 83 cases and 208
assertions, public API calls, fixture values, headers and Meson remain intact.

The new case fails at its close-error assertion when only the copier is restored
to its old body, and passes with the fix. Seven existing positive sizes (0, 1,
4095, 4096, 4097, 8192, 8193) still copy identical bytes. Fresh private CPU
release builds pass both existing binaries with DNN disabled and actual ORT
1.29.0 enabled. Whole-file strict clang-tidy and exhaustive POSIX Cppcheck pass;
the actual one-TU scoped writer leaves the existing baseline byte-identical.
No new suppression, ADR, production change or golden-data edit is needed.

Exact commands, old/fixed controls, source and tool identities, raw logs and
normal hooks are retained under
`.workingdir2/evidence/dnn-copy-flush-20260908/`; build output stays in the
matching cache. This bounded follow-up makes no fresh whole-tree, GPU,
Windows/Darwin, sanitizer or Netflix golden acceptance claim.
