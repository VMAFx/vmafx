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
