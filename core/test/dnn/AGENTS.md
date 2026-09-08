# AGENTS.md — core/test/dnn

Parent rules: [../AGENTS.md](../AGENTS.md).

## Tensor I/O regression invariants

`test_tensor_io` compiles the real `core/src/dnn/tensor_io.c` directly and runs
even with `enable_dnn=disabled`; it needs neither ONNX Runtime nor a GPU. Keep
read-only input fixtures const and retain all valid-input, special-value,
round-trip, layout and error-path assertions when refactoring test drivers.
Group drivers by topic to stay within the existing function-size threshold;
call each real test once and propagate its first failure without counting helper
groups as extra tests.

The six explicit unsupported dtype/resize casts exercise the documented
`tensor_io.h` rejection contract. Preserve those actual invalid values and their
assertions. Their individual `EnumCastOutOfRange` markers follow ADR-1080's
invalid-enum-test invariant; do not replace them with valid values, opaque data
construction, or a file-wide suppression. See the
[tensor test cleanup digest](../../../docs/research/tensor-io-test-cleanup-2026-09-08.md).

## ORT/session test fixtures (Research-2052)

`test_ort_internals.c` keeps its original 48-case table and read-only inference
inputs/shapes. `test_dnn_session_api.c` keeps all 35 original registrations in
order; its private driver groups propagate the first failure without counting
themselves as cases. Original assertions, negative dimensions, fixture values,
API calls and ownership remain unchanged. The additional POSIX copy-error case
must fail with the old helper: stop after a short read and inspect `ferror`
before reporting a fixture copy as successful. Also check destination `fclose`:
a buffered write may fail only on final flush. Keep the child-only close-error
case first, before any ORT initialization or threads; only the child changes
`RLIMIT_FSIZE`/`SIGXFSZ`, and setup failure is distinct from an incorrect copy
result. The read-error case remains last. Keep the measured zero warning
entries and existing ADR-1138 C `NULL` brackets. See
[Research-2052](../../../docs/research/2052-dnn-tests-native-lint.md).
