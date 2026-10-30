# Language bindings

The VMAFx C API is defined once, in `core/api/vmafx.toml`; the C headers, this
reference and the language bindings are generated from that definition. This
page says which language bindings exist today, which API each one wraps and how
to load it.

| Language | Package in the source tree | Wraps | Generated from `vmafx.toml` |
| :--- | :--- | :--- | :--- |
| Python | `bindings/python/vmafx/` | VMAFx API (`libvmafx.so.1`) | yes |
| Rust | `bindings/rust/vmafx-sys/`, `bindings/rust/vmafx/` | VMAFx API (`libvmafx.so.1`) and `libvmaf.h` compatibility API (`libvmaf.so.3`) | yes |
| Go | `pkg/vmafx/` | VMAFx API (`libvmafx.so.1`) | yes |

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

Two crates sit in `bindings/rust/`. The VMAFx C API binding is generated from
`core/api/vmafx.toml` by `scripts/codegen/vmafx_api/emit_rust.py` into three
files alongside the existing `libvmaf.h` compatibility surface:

- `bindings/rust/vmafx-sys/src/vmafx_api_gen.rs` (`vmafx_sys::vmafx_api`):
  `#[repr(C)]` struct definitions, opaque handles, status and enum constants,
  `unsafe extern "C"` declarations for `libvmafx.so.1`, and compile-time
  `core::mem::size_of` / `core::mem::align_of` assertions for every struct in
  `core/api/vmafx.toml`.
- `bindings/rust/vmafx/src/status_gen.rs` (`vmafx::Status`): status enum and
  `POSIX` errno mapping, usable without linking the C library.
- `bindings/rust/vmafx/src/api_gen.rs` (`vmafx::api_gen`): safe RAII `Context`,
  owned Rust record structs, structured `Error` (`std::error::Error` and
  `Display`), and package-level library functions.

The legacy `libvmaf.h` bindgen surface in `vmafx-sys` and the phase-1 `libvmaf`
wrappers at the crate root of `vmafx`
([ADR-0929](../../adr/0929-rust-safe-binding-scaffold.md),
[README](https://github.com/VMAFx/vmafx/blob/master/bindings/rust/vmafx/README.md))
remain available while callers migrate (see the [migration table](compat.md)).

### Linking the library

`vmafx-sys/build.rs` searches `${LIBVMAF_PREFIX}` first, then `/usr/local` and
`/usr`, and links both `libvmaf` and `libvmafx` (`-lvmaf -lvmafx`). Point
`LIBVMAF_PREFIX` at your VMAFx installation prefix and `LD_LIBRARY_PATH` at its
library directory (setting `DOCS_RS=1` compiles against the in-tree
`core/include` headers without native linking):

```bash
export LIBVMAF_PREFIX="/usr/local"
export LD_LIBRARY_PATH="${LIBVMAF_PREFIX}/lib"
```

### Scoring

Create a `vmafx::api_gen::Context` with `Context::new(config)` (or
`Context::new(None)` for defaults) and release it with `close()` or `Drop`:

```rust
use vmafx::api_gen::{Context, Error, Pool};

fn main() -> Result<(), Error> {
    let ctx = Context::new(None)?;
    ctx.import_score("external", 0, 1.5)?;
    let pooled = ctx.feature_score_pooled("external", Pool::MEAN, 0, 0)?;
    println!("{} {}", pooled.feature, pooled.value); // external 1.5
    ctx.close()?;
    Ok(())
}
```

`Context::close(self)` and `Drop` honour the retry-safe teardown contract
([ADR-1336](../../adr/1336-cuda-context-owned-resource-teardown.md)): if
`vmafx_context_destroy` returns a non-OK status, `close()` returns
`Err((ctx, err))` handing the live `Context` back so teardown may be retried,
while `Drop` retries once and never panics.

### Errors

Every non-OK C call reads and frees the `VmafxError` diagnostic and returns a
`vmafx::api_gen::Error` carrying `function`, `status` (`vmafx::Status`),
`message`, `subject` and `errno`. Calls that return `VMAFX_PENDING` map to
`Status::Pending` (`err.is_pending()`):

```rust
use vmafx::Status;
use vmafx::api_gen::Pool;

match ctx.feature_score_pooled("external", Pool::MEAN, 0, 10) {
    Ok(pooled) => println!("{}", pooled.value),
    Err(err) if err.status == Status::Pending => {
        // not every frame is scored yet; ask again later
    }
    Err(err) => eprintln!("{} {}: {}", err.status.as_str(), err.subject, err.message),
}
```

### What is not wrapped yet

Like the Python and Go bindings, `vmafx::api_gen` wraps the context methods and
library functions whose parameters are plain scalar values and output structs.
Calls that take device handles, callbacks or other handle types remain on the
raw `vmafx_sys::vmafx_api` and `vmafx_sys` FFI layer; `ctx.libvmaf_handle()`
returns the underlying `*mut vmafx_sys::VmafContext` (and `ctx.as_raw()` returns
`*mut vmafx_sys::vmafx_api::VmafxContext`) for calls not yet migrated.

### Tests

Header, library-search and status tests run under `DOCS_RS=1` without a built C
library; full integration tests in `bindings/rust/vmafx/tests/api_gen.rs` run
against `libvmafx.so.1`:

```bash
DOCS_RS=1 cargo test -p vmafx-sys --test header_check --test library_search
DOCS_RS=1 cargo test -p vmafx --test status
LIBVMAF_PREFIX="/usr/local" LD_LIBRARY_PATH="/usr/local/lib" \
  cargo test -p vmafx --test api_gen
```

## Go

The Go package `github.com/VMAFx/vmafx/pkg/vmafx` is generated from
`core/api/vmafx.toml` by `scripts/codegen/vmafx_api/emit_go.py` into two files:

- `pkg/vmafx/status_gen.go`: pure-Go `Status` constants and error sentinels
  (`ErrInvalid`, `ErrNotFound`, `ErrNotSup`, and the rest of the status table),
  usable with `CGO_ENABLED=0`.
- `pkg/vmafx/api_gen.go` (`//go:build cgo`): cgo binding of `libvmafx.so.1`
  with compile-time `unsafe.Sizeof` and `unsafe.Alignof` checks for every struct
  in `core/api/vmafx.toml`, Go record types, `Context`, `Model`, `Frame` and
  package-level library functions.

### Linking the library

`pkg/vmafx` sets `#cgo CFLAGS: -I${SRCDIR}/../../core/include` and intentionally
defines no `#cgo pkg-config` or implicit `-L` search fallback ([ADR-1125](../../adr/1125-vmafx-tune-go-port-integration.md)):
static analysis (`go vet`) runs against the in-tree headers without a pre-built
shared library, while `go build` and `go test` require `CGO_LDFLAGS` and the
runtime library path pointing at your VMAFx build or install directory:

```bash
export CGO_ENABLED=1
export CGO_LDFLAGS="-L/path/to/build/src -lvmafx"
export LD_LIBRARY_PATH="/path/to/build/src"
```

### Scoring

Create a `Context` with `vmafx.New(config)` (or `vmafx.New(nil)` for defaults)
and release it with `Close()`:

```go
package main

import (
    "fmt"
    "log"

    "github.com/VMAFx/vmafx/pkg/vmafx"
)

func main() {
    ctx, err := vmafx.New(nil)
    if err != nil {
        log.Fatal(err)
    }
    defer func() {
        if err := ctx.Close(); err != nil {
            log.Fatal(err)
        }
    }()

    if err := ctx.ImportScore("external", 0, 1.5); err != nil {
        log.Fatal(err)
    }
    pooled, err := ctx.FeatureScorePooled("external", vmafx.PoolMean, 0, 0)
    if err != nil {
        log.Fatal(err)
    }
    fmt.Println(pooled.Feature, pooled.Value) // external 1.5
}
```

`Close()` honours the retry-safe teardown contract ([ADR-1336](../../adr/1336-cuda-context-owned-resource-teardown.md)):
if `vmafx_context_destroy` returns a non-OK status, the `Context` handle stays
open so `Close()` may be retried; once `Close()` succeeds, subsequent `Close()`
calls are no-ops returning `nil`.

#### Scoring frames

`Model` and `Frame` are refcounted handles released with `Close()` (safe on nil
and to call twice). A host frame is filled through `PlaneBytes`, which aliases
the frame's C memory, and handed to the context with `Submit`, which consumes
both frames on every path, so a `Close()` after `Submit` is a no-op:

```go
model, err := vmafx.LoadModelFile(&vmafx.ModelConfig{Name: "vmaf"}, "model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json")
if err != nil {
    log.Fatal(err)
}
defer model.Close()
if err := ctx.UseModel(model); err != nil {
    log.Fatal(err)
}

desc := vmafx.FrameDesc{PixFmt: 1, Bpc: 8, W: 576, H: 324} // 4:2:0, 8 bit
for index := uint64(0); index < frames; index++ {
    ref, err := vmafx.NewHostFrame(desc)
    if err != nil {
        log.Fatal(err)
    }
    dis, err := vmafx.NewHostFrame(desc)
    if err != nil {
        ref.Close()
        log.Fatal(err)
    }
    // ... copy each plane row into ref.PlaneBytes(p) and dis.PlaneBytes(p), honouring Planes().Stride ...
    if err := ctx.Submit(ref, dis, index); err != nil {
        log.Fatal(err)
    }
}
if err := ctx.Flush(); err != nil {
    log.Fatal(err)
}
mean, err := ctx.ScorePooled(model, vmafx.PoolMean, 0, frames-1)
```

`ProvenanceJson(0)` returns the record the CLI report embeds, as JSON text.

#### pkg/libvmaf runs on this binding

The in-process paths of `pkg/libvmaf` (`ScoreDirect`, used by `vmafx-mcp` with
`VMAFX_MCP_DIRECT=1`, and `StreamScorer`, used by the `vmafx-server` gRPC
`ScoreStream`) hold no cgo of their own since [#2434](https://github.com/VMAFx/vmafx/issues/2434):
they call `LoadModelFile`, `UseModel`, `NewHostFrame`, `Submit`, `Flush` and
`ScorePooled` of this package. The pooled mean of `vmaf_v1.0.16_3d0h` through
`ScoreDirect`, through `pkg/vmafx` and through the libvmaf C API is bit-for-bit
the same value (`TestPooledMeanBitExactAcrossBindings`, compared as `%x`).
`Scorer` still runs the `vmaf` CLI as a subprocess; the CLI is a process
boundary this in-process binding does not replace. `DNNSession` keeps its own
cgo file because the binding has no DNN session handle yet.

The errno contract of `pkg/libvmaf` is kept. `pkg/vmafx` reports a stable
`Status` in a `*vmafx.Error`; `pkg/libvmaf` maps `StatusEInvalid`,
`StatusENoMem`, `StatusENotFound` and `StatusEIO` to `ErrInvalidArgument`
(wraps `os.ErrInvalid`), `ErrOutOfMemory`, `ErrModelNotFound` (wraps
`os.ErrNotExist`) and `ErrPictureRead`, and keeps the `*vmafx.Error` in the chain
for `errors.As`. A missing model file is a file error (`StatusEIO`) in `vmafx`;
`pkg/libvmaf` checks the path again and still returns `ErrModelNotFound`.

### Errors

Every non-OK C call reads and frees the `VmafxError` diagnostic and returns a
`*vmafx.Error` carrying `Function`, `Status`, `Message`, `Subject` and `Errno`.
`Error.Unwrap()` maps negative statuses to the sentinels in `status_gen.go`
(`ErrInvalid`, `ErrNotFound`, `ErrNotSup`, which in turn wrap `os.ErrInvalid`,
`os.ErrNotExist` and `errors.ErrUnsupported`) and maps `StatusPending`
(`VMAFX_PENDING`) to `vmafx.ErrPending`:

```go
pooled, err := ctx.FeatureScorePooled("external", vmafx.PoolMean, 0, 10)
if errors.Is(err, vmafx.ErrPending) {
    // not every frame is scored yet; ask again later
} else if err != nil {
    var vErr *vmafx.Error
    if errors.As(err, &vErr) {
        fmt.Println(vErr.Status, vErr.Subject, vErr.Message)
    }
}
```

### What is not wrapped yet

Like the Python binding, `pkg/vmafx` wraps the context methods and library
functions whose parameters are plain scalar values, strings, output structs and
the `Model` and `Frame` handles. Calls that take device handles, model sets,
option lists, callbacks (windows) or imported frames remain on the C layer;
`(c *Context) LibvmafHandle()` returns the underlying `VmafContext *` as an
`unsafe.Pointer` for calls not yet migrated. For remote scoring over gRPC or
REST, `gen/go/` (module `github.com/VMAFx/vmafx/gen/go`, package `vmafxv1`)
provides the generated client and server stubs.

### Tests

Pure-Go status tests run without a built C library; full cgo tests run against
`libvmafx.so.1`:

```bash
CGO_ENABLED=0 go test -v ./pkg/vmafx/...
CGO_ENABLED=1 CGO_LDFLAGS="-L$(pwd)/core/build-cpu/src -lvmaf -lvmafx -lm" \
  LD_LIBRARY_PATH="$(pwd)/core/build-cpu/src" \
  go test -v -race -count=1 ./pkg/vmafx/...
```

Every function `api_gen.go` wraps has a test against the built library:
`TestEveryWrappedFunctionHasATest` reads the generated file and fails for a
wrapped function no test file calls. The model and frame tests read
`model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json` and the tracked `testdata/` pair; a
missing file skips the test with its path named. The bit-exactness test of
`pkg/libvmaf` also compiles `pkg/libvmaf/testdata/c_api_pooled_mean.c` with `cc`
against the library `CGO_LDFLAGS` selects, and reads the Netflix 576x324 pair
from `python/test/resource/yuv/` (override with `VMAFX_YUV_REF` and
`VMAFX_YUV_DIS`).

## See also

- [VMAFx API preview](index.md): context lifecycle, scoring and provenance in C
- [Generated reference](reference.md): every function, struct and enum
- [API generation](../../development/api-generation.md): how the headers and
  the Python binding are generated from `core/api/vmafx.toml`
- [libvmaf migration table](compat.md): the VMAFx successor of every
  `libvmaf.h` function
