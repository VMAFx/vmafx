# Language bindings

The VMAFx C API is defined once, in `core/api/vmafx.toml`; the C headers, this
reference and the Python binding are generated from that definition. This page
says which language bindings exist today, which API each one wraps and how to
load it. Read it before choosing a binding: the three languages are not at the
same stage.

| Language | Package in the source tree | Wraps | Generated from `vmafx.toml` |
| :--- | :--- | :--- | :--- |
| Python | `bindings/python/vmafx/` | VMAFx API (`libvmafx.so.1`) | yes |
| Rust | `bindings/rust/vmafx-sys/`, `bindings/rust/vmafx/` | `libvmaf.h` compatibility API (`libvmaf.so.3`) | no |
| Go | none for the C library | (gRPC and REST stubs in `gen/go/`, for the server) | no |

Generated Rust and Go bindings of the VMAFx API are tracked in
[issue 2434](https://github.com/VMAFx/vmafx/issues/2434).

## Python

The Python binding uses only the standard library (`ctypes`). Every class,
enum and method in it is generated into `vmafx._api` by
`scripts/codegen/vmafx_api/emit_python.py`; the package re-exports the names
most programs need.

### Loading the library

The binding loads exactly the library you name and never searches for one.
Pass the path of `libvmafx.so.1`, or set `VMAFX_LIBRARY` and call
`vmafx.load()`:

```python
from vmafx import Library

lib = Library("/usr/local/lib/libvmafx.so.1")
print(lib.version_string(), lib.abi_version())
```

`Library()` without a path reads `VMAFX_LIBRARY`; when it is not set, the call
raises `OSError` naming the variable. Do not pass `libvmaf.so.3`: that is the
compatibility library and does not export the `vmafx_*` functions.

When the module is imported it checks every struct layout against the
definition. `vmafx.ABI_VERSION` is the version the binding was generated for;
`Library` does not compare it with the loaded library, so a program that must
refuse a mismatch compares `lib.abi_version()` with it itself (see
[`vmafx/version.h`](version.md)).

### Scoring

A context is a context manager; leaving the `with` block destroys it:

```python
from vmafx import Library, Pool

lib = Library("/usr/local/lib/libvmafx.so.1")
with lib.context() as context:
    context.import_score("external", 0, 1.5)
    pooled = context.feature_score_pooled("external", Pool.MEAN, 0, 0)
print(pooled.feature, pooled.value)   # external 1.5
```

`lib.context(config)` takes a `vmafx.ContextConfig` (log level, thread count,
subsampling, CPU and GPU masks, import retry wait); without one the library
defaults apply.
`feature_score_pooled()` returns a `PooledScore` with `feature`, `pool`,
`first`, `last` and `value`. `context.provenance()` returns the provenance
record of the context (version, commit, build flags, floating-point policy,
active backend and device, SIMD level); see
[`vmafx/provenance.h`](provenance.md).

### Errors

A failed call raises `vmafx.VmafxError`, which carries the fields of the C
error object: `function`, `status`, `message`, `subject` and `errno`. A call
that returns `VMAFX_PENDING` raises its subclass `vmafx.VmafxPending`, so a
caller that polls a window or a fence catches that one first:

```python
from vmafx import VmafxError, VmafxPending

try:
    pooled = context.feature_score_pooled("external", Pool.MEAN, 0, 10)
except VmafxPending:
    pass            # not every frame is scored yet; ask again later
except VmafxError as error:
    print(error.status, error.subject, error.message)
```

The status codes and subjects are the ones of the C API; see the status table
in [`vmafx/types.h`](types.md#status-codes) and [`vmafx/error.h`](error.md).

### What is not wrapped yet

The binding wraps the functions whose arguments are plain values. The others,
for example frame import with device handles, are reachable through
`Library.raw`, the underlying `ctypes.CDLL`; loading has already set the
argument and result types of every function from the definition. The
generated module `vmafx._api` holds every enum and `ctypes.Structure` of the C
API under its C name (for example `vmafx._api.VmafxContextConfig`).

### Tests

`core/test/test_vmafx_python_binding.py` runs in the `fast` Meson suite
against the library of the same build, with `VMAFX_LIBRARY` set by Meson:

```bash
python3 scripts/ci/run_meson_test.py -- -C build test_vmafx_python_binding
```

## Rust

Two crates sit in `bindings/rust/`. Both wrap the `libvmaf.h` compatibility
API, not the VMAFx API, and neither is generated from `vmafx.toml` yet.

- `vmafx-sys` holds the raw FFI, produced by bindgen from
  `${LIBVMAF_PREFIX}/include/libvmaf/libvmaf.h` and linked against
  `${LIBVMAF_PREFIX}/lib/libvmaf.so` (`LIBVMAF_PREFIX` defaults to
  `/usr/local`).
- `vmafx` is the safe layer (phase 1 of
  [ADR-0929](../../adr/0929-rust-safe-binding-scaffold.md)): `Context`, `Model`,
  `Picture`, `Score`, `PoolingMethod` and a `Result<T, vmafx::Error>`.
  `Context::read_pictures()` takes ownership of the pictures, `Context` is not
  `Send`, and `Model` and `Picture` are `Send` but not `Sync`.

Usage, ownership rules and the example program are in the crate's
[README](https://github.com/VMAFx/vmafx/blob/master/bindings/rust/vmafx/README.md).
Code written against these crates keeps working while the compatibility
library ships; it moves to the generated binding of
[issue 2434](https://github.com/VMAFx/vmafx/issues/2434) when that lands,
following the [migration table](compat.md).

## Go

There is no Go binding of the C library. A Go program reaches VMAFx through
the scoring server instead: `gen/go/` (module `github.com/VMAFx/vmafx/gen/go`,
package `vmafxv1`) holds the gRPC and REST client stubs generated from
`proto/` by `scripts/codegen/proto_generate.py`. Their `ScoreOptions` and
`Provenance` messages come from the same `vmafx.toml`, so they carry the same
fields as the C structs. A cgo binding of the C API is part of
[issue 2434](https://github.com/VMAFx/vmafx/issues/2434).

## See also

- [VMAFx API preview](index.md): context lifecycle, scoring and provenance in C
- [Generated reference](reference.md): every function, struct and enum
- [API generation](../../development/api-generation.md): how the headers and
  the Python binding are generated from `core/api/vmafx.toml`
- [libvmaf migration table](compat.md): the VMAFx successor of every
  `libvmaf.h` function
