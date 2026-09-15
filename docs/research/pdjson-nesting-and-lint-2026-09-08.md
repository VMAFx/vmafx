# pdjson nesting boundary and parser lint cleanup

## Reproduced defect

The fork's [ADR-1061](../adr/1061-vendored-cjson-pdjson-depth-overflow.md) selects
a 512-container nesting limit. The old parser initialized `stack_top` to -1,
incremented it on entry to `push()`, then rejected only indices greater than 512.
It therefore accepted 513 nested containers. It also published the next depth
before determining whether allocation succeeded.

The regression exercises 511, 512 and 513 levels under ASan/UBSan. The old
source passes the first two and fails the expected rejection at 513. The fixed
source checks the next index against the container count before allocation and
publishes the new stack top only after success. Rejection leaves accepted depth
unchanged and retains the existing error message. This is a bug fix under the
existing decision, not a new depth policy; no new ADR is needed.

A separate compiled control reproduces a heap-buffer-overflow with the old
`PDJSON_STACK_INC=0` override: the allocator returns a zero-sized allocation and
`push()` writes its first frame. The fix rejects zero and oversized increments
before allocator invocation. The normal increment remains four.

## Preserved parser contracts

The parser remains the vendored [skeeto/pdjson](https://github.com/skeeto/pdjson)
C implementation under its Unlicense notice. Its upstream API supports a pull
event stream, optional strict document completion, raw or escaped UTF-8,
embedded NULs, custom input callbacks and custom allocators. Those mechanisms
remain in place; none of the previously unexercised private API functions is
deleted to satisfy an unused-function warning.

The new `test_pdjson` fast test covers event ordering, object context/counts,
skip operations, cached lookahead, streaming separators and reset, file and user
sources, embedded-NUL/Unicode decoding, 25 malformed documents, four allocation
failure points plus success, and the nesting boundary. Two separately compiled
controls reject zero and oversized stack-growth overrides before allocation.
Existing model and model
ownership tests exercise the real C++ model-loader consumer as well.

## Lint correction

The full configured run retained 24 Cppcheck findings in `pdjson.c`. Loading the
official POSIX model does not fix this parser's scope, const, shadowing or unused
API findings. Removing its uncited blanket NOLINT exposes 69 distinct configured
clang-tidy findings. The cleanup keeps the original formatter and first-error
guard, explicitly discards best-effort formatting results, names the zero
lookahead sentinel without changing existing event values, splits number/object
parsing phases, simplifies UTF-8 validation and const-qualifies read-only private
getters. A single cited ADR-1138 bracket retains C/upstream `NULL` and Windows
MSVC compatibility; no other warning category is suppressed.

No alternatives: accepting 513 levels contradicts the existing limit; removing
private APIs or keeping the blanket suppression would discard useful behavior
and hide diagnostics. The compiler database and analysis categories stay intact.

## Reproducer and evidence

```sh
meson test -C build --print-errorlogs test_pdjson \
  test_pdjson_stack_increment_zero test_pdjson_stack_increment_oversized \
  test_model test_model_libsvm_dup_key test_model_feature_overload_ownership
```

Canonical receipts live at
`.workingdir2/evidence/pdjson-lint-20260908`: before/fixed source and command
hashes, ASan/UBSan logs, configured analyzer diagnostics, behavioral comparison,
and generated scoped baseline provenance. The six parser/model Meson nodes pass
under ASan/UBSan. A 131,095-input old/new trace comparison covers every two-byte
quoted input, every BMP Unicode escape and representative JSON values, with
identical events, decoded bytes, errors and input positions. The measured parser
and test TUs have zero clang-tidy diagnostics; the scoped baseline removes one
uncited NOLINT without changing warning allowances or unmeasured entries.
Normal Cppcheck emits only branch-budget information (exit 1). Repeating the
same 14 parser/test/caller commands with `--check-level=exhaustive` resolves that
budget limit and exits 0; no diagnostic category is disabled.
C99 syntax validation also passes after removing the unnecessary POSIX feature
macro: this parser uses only standard C facilities.

Builds use a private CPU-only cached
container and output directory. These bounded checks do not claim a fresh
whole-tree `make lint`/`make test`, Windows runtime or release acceptance.
