# AGENTS.md — core/test/dnn

Parent rules: [../AGENTS.md](../AGENTS.md).

## Tensor I/O regression invariants

`test_tensor_io` compiles real `core/src/dnn/tensor_io.c` directly and runs
even with `enable_dnn=disabled`; needs neither ONNX Runtime nor GPU. Keep
read-only input fixtures const and retain all valid-input, special-value,
round-trip, layout and error-path assertions when refactoring test drivers.
Group drivers by topic to stay within existing function-size threshold;
call each real test once and propagate its first failure without counting
helper groups as extra tests.

Six explicit unsupported dtype/resize casts exercise documented
`tensor_io.h` rejection contract. Preserve those actual invalid values and
their assertions. Their individual `EnumCastOutOfRange` markers follow
ADR-1080's invalid-enum-test invariant; do not replace them with valid
values, opaque data construction, or file-wide suppression. See
[tensor test cleanup digest](../../../docs/research/tensor-io-test-cleanup-2026-09-08.md).

## ORT/session test fixtures (Research-2052)

`test_ort_internals.c` keeps its original 48-case table and read-only
inference inputs/shapes. `test_dnn_session_api.c` keeps all 35 original
registrations in order; its private driver groups propagate first failure
without counting themselves as cases. Original assertions, negative
dimensions, fixture values, API calls and ownership remain unchanged. The
additional POSIX copy-error case must fail with old helper: stop after
short read and inspect `ferror` before reporting fixture copy as
successful. Also check destination `fclose`: buffered write may fail only
on final flush. Keep child-only close-error case first, before ORT
initialization or threads; only child changes `RLIMIT_FSIZE`/`SIGXFSZ`, and
setup failure is distinct from incorrect copy result. Read-error case
remains last. Keep measured zero warning entries and existing ADR-1138 C
`NULL` brackets. See
[Research-2052](../../../docs/research/2052-dnn-tests-native-lint.md).
