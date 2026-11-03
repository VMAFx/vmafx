<!-- Copyright 2026 Lusoris -->
<!-- SPDX-License-Identifier: EUPL-1.2 -->

# VMAFx Python package

The `vmafx` Python package (`bindings/python/`) provides Python bindings for
the modernized VMAFx C API (`libvmafx.so.1`).

Every class, enum, record dataclass, and method in `vmafx` is generated directly
from the single-source API definition in [`core/api/vmafx.toml`](https://github.com/VMAFx/vmafx/blob/master/core/api/vmafx.toml)
by `scripts/codegen/vmafx-api.py` ([ADR-1852](../adr/1852-vmafx-api-redesign.md)).
The generated code lives in `bindings/python/vmafx/_api.py`, and
[`bindings/python/vmafx/__init__.py`](https://github.com/VMAFx/vmafx/blob/master/bindings/python/vmafx/__init__.py)
re-exports the public API.

The package uses only the Python standard library (`ctypes`). It has zero
runtime dependencies (`dependencies = []` in [`bindings/python/pyproject.toml`](https://github.com/VMAFx/vmafx/blob/master/bindings/python/pyproject.toml)),
ships a `py.typed` marker file ([`bindings/python/vmafx/py.typed`](https://github.com/VMAFx/vmafx/blob/master/bindings/python/vmafx/py.typed))
for PEP 561 type checkers, and supports zero-copy frame ingestion from contiguous
buffer-protocol objects whose planes match a supported `FrameDesc` geometry.

On import, `_check_layout()` in `bindings/python/vmafx/_api.py` validates the
size and field offsets of the generated `LAYOUT` mapping, raising `ImportError`
on any discrepancy before the native library is loaded. The mapping is generated
from `core/api/vmafx.toml`.

!!! note "Looking for model training or legacy CLI scripts?"
    This page documents the modern `vmafx` package for scoring video via
    `libvmafx.so.1`. If you need model training, dataset validation, or legacy
    tools from the original Netflix Python codebase (`python/`), see the
    [VMAF Python library](python.md) guide.

## Supported Python versions

The package requires Python 3.12 or higher, declared in
[`bindings/python/pyproject.toml`](https://github.com/VMAFx/vmafx/blob/master/bindings/python/pyproject.toml):

```toml
requires-python = ">=3.12"
```

The binding relies on standard-library features available in Python 3.12 and
later, including union type syntax (`X | Y`) and enhanced buffer protocol
support.

## Installation from source

Today, the package must be installed from source. First compile and install
the native `libvmafx` library with Meson, then install the Python package
into your virtual environment:

```bash
# 1. Build and install libvmafx with Meson
meson setup build core
ninja -C build
sudo ninja -C build install

# 2. Install the Python binding from the repository tree
pip install ./bindings/python
```

Alternatively, you can build a wheel locally using standard build isolation,
or install `hatchling>=1.32.0` first if building without build isolation:

```bash
# Build with build isolation (pip installs hatchling into an isolated build environment)
python3 -m pip wheel --no-deps -w dist bindings/python
pip install dist/vmafx-*.whl

# Or install hatchling first when building without build isolation
pip install "hatchling>=1.32.0"
python3 -m pip wheel --no-deps --no-build-isolation -w dist bindings/python
pip install dist/vmafx-*.whl
```

## How the package finds `libvmafx`

The binding does not perform its own library search or probe directories;
`load()` passes the chosen path directly to `ctypes.CDLL()`. When given an
explicit filesystem path, that exact file is loaded. When given a bare soname
(such as `libvmafx.so.1`), resolution is delegated to the operating system
dynamic loader using its standard search paths (`LD_LIBRARY_PATH`, system library
cache, or platform equivalent).

In [`bindings/python/vmafx/_api.py`](https://github.com/VMAFx/vmafx/blob/master/bindings/python/vmafx/_api.py),
the loader function `load()` defines how the shared library is resolved:

```python
def load(path: str | None = None) -> ctypes.CDLL:
    """Load the library from `path` or $VMAFX_LIBRARY. There is no search fallback."""
    chosen = path or os.environ.get("VMAFX_LIBRARY")
    if not chosen:
        raise OSError("set VMAFX_LIBRARY or pass the path of the VMAFx shared library")
    lib = ctypes.CDLL(chosen)
...
```

You can point the loader to the compiled library in two ways:

1. **Pass the path explicitly** when creating a `Library` instance:

    ```python
    from vmafx import Library

    lib = Library("/usr/local/lib/libvmafx.so.1")
    ```

2. **Set the `VMAFX_LIBRARY` environment variable** before invoking Python:

    ```bash
    export VMAFX_LIBRARY=/usr/local/lib/libvmafx.so.1
    ```

    ```python
    from vmafx import Library

    lib = Library()  # Reads $VMAFX_LIBRARY automatically
    ```

If neither `path` nor `VMAFX_LIBRARY` is set, `load()` raises:

```text
OSError: set VMAFX_LIBRARY or pass the path of the VMAFx shared library
```

!!! warning "Do not pass `libvmaf.so.3`"
    Pass the path of `libvmafx.so.1` (or on macOS `libvmafx.dylib`, on Windows
    `vmafx.dll`). Do not pass `libvmaf.so.3`: that is the compatibility wrapper
    library for older Netflix C APIs and does not export the `vmafx_*` entry
    points.

## Minimal scoring example

Every resource-owning class in `vmafx` (`Context`, `Model`, `ModelSet`,
`Device`, `Frame`, `FramePool`, `FrameConverter`, `Options`, `ReportFile`,
`Window`, `WindowClock`, `DnnSession`, `McpServer`) implements the
context-manager protocol (`with` statement) for deterministic resource
cleanup. (`Library` wraps the loaded shared library and is not a context
manager).

### Basic score import and pooling

Here is a minimal example using `Library`, `Context`, `Model`, `Pool`, and
`PooledScore` without external video files:

```python
from vmafx import Library, Pool

# Load the library using $VMAFX_LIBRARY
lib = Library()

with lib.model_load("vmaf_v0.6.1") as model, lib.context() as ctx:
    ctx.use_model(model)
    ctx.import_score("external", 0, 1.5)
    pooled = ctx.feature_score_pooled("external", Pool.MEAN, 0, 0)
    print(f"Feature: {pooled.feature}, Score: {pooled.value}")
```

### Video frame scoring with buffer-protocol frames

To score actual video frames, describe the geometry with `FrameDesc` and wrap
in-memory planar buffers (`bytes`, `bytearray`, `memoryview`, or `numpy`
arrays) using `Library.frame_wrap_host()` (or `Frame.wrap_host()`):

```python
from vmafx import (
    Color,
    ColorMatrix,
    ColorPrimaries,
    ColorRange,
    ColorTransfer,
    FrameDesc,
    Library,
    PixelFormat,
    Pool,
)

lib = Library("/usr/local/lib/libvmafx.so.1")

color = Color(
    range=ColorRange.LIMITED,
    primaries=ColorPrimaries.BT709,
    trc=ColorTransfer.BT709,
    matrix=ColorMatrix.BT709,
)

desc = FrameDesc(
    pix_fmt=PixelFormat.YUV420P,
    bpc=8,
    w=64,
    h=64,
    color=color,
)

with lib.model_load("vmaf_v0.6.1") as model, lib.context() as ctx:
    ctx.use_model(model)
    ctx.use_feature("psnr")

    # Ingest planar data for frame 0 (Y, U, V)
    y_ref = b"\x80" * (64 * 64)
    u_ref = b"\x80" * (32 * 32)
    v_ref = b"\x80" * (32 * 32)

    y_dist = b"\x7f" * (64 * 64)
    u_dist = b"\x7f" * (32 * 32)
    v_dist = b"\x7f" * (32 * 32)

    with (
        lib.frame_wrap_host(desc, (y_ref, u_ref, v_ref)) as ref_frame,
        lib.frame_wrap_host(desc, (y_dist, u_dist, v_dist)) as dist_frame,
    ):
        ctx.submit(ref_frame, dist_frame, 0)

    ctx.flush()

    vmaf = ctx.score_pooled(model, Pool.MEAN, 0, 0)
    print(f"VMAF score: {vmaf.value:.6f}")

    psnr = ctx.feature_score_pooled("psnr_y", Pool.MEAN, 0, 0)
    print(f"PSNR_Y score: {psnr.value:.6f}")
```

The underlying functions used above match the verified symbols in
`bindings/python/vmafx/_api.py` and
`core/test/test_vmafx_python_binding.py`:

- `Library.model_load()` (`vmafx_model_load`)
- `Library.context()` (`vmafx_context_create`)
- `Library.frame_wrap_host()` (`vmafx_frame_wrap_host`)
- `Context.use_model()` (`vmafx_context_use_model`)
- `Context.use_feature()` (`vmafx_context_use_feature`)
- `Context.submit()` (`vmafx_submit`)
- `Context.flush()` (`vmafx_flush`)
- `Context.score_pooled()` (`vmafx_score_pooled`)
- `Context.feature_score_pooled()` (`vmafx_feature_score_pooled`)

## Error handling

Failed library calls raise exceptions defined in
[`bindings/python/vmafx/_api.py`](https://github.com/VMAFx/vmafx/blob/master/bindings/python/vmafx/_api.py):

- **`vmafx.VmafxError`**: Base exception raised whenever a C function returns
  a non-zero status code (except `WindowClock.frame()` and `WindowClock.finish()`,
  which return `None` on `Status.PENDING`). It extracts detailed diagnostic fields
  from the C error handle (`core/include/vmafx/error.h`) and frees the handle:
  - `error.function`: the C or binding function name that failed
    (for example `"vmafx_model_load"`).
  - `error.status`: integer status code corresponding to the `vmafx.Status`
    enum (`bindings/python/vmafx/_api.py`), such as `Status.E_INVALID` (-1),
    `Status.E_NOMEM` (-2), `Status.E_NOTSUP` (-3), `Status.E_NOTFOUND` (-4),
    `Status.E_BUSY` (-5), `Status.E_RANGE` (-8), or `Status.E_TIMEOUT` (-10).
  - `error.message`: explanatory error text returned by
    `vmafx_error_message()`.
  - `error.subject`: name of the offending argument or resource
    (`vmafx_error_subject()`).
  - `error.errno`: POSIX `errno` integer from `vmafx_error_errno()`.
  - `error.subject_kind`: integer subject classification from
    `vmafx_error_subject_kind()`.

- **`vmafx.VmafxPending`**: A subclass of `VmafxError` raised when a scoring
  call returns `Status.PENDING` (status code `1`). This indicates that the
  requested score is not yet computed (for example, in asynchronous window
  scoring before enough frames are submitted). Note that tumbling window clock
  methods (`WindowClock.frame()` and `WindowClock.finish()`) return `None` rather
  than raising `VmafxPending` when a window span is pending.

```python
from vmafx import Pool, VmafxError, VmafxPending

try:
    pooled = ctx.feature_score_pooled("psnr_y", Pool.MEAN, 0, 10)
except VmafxPending:
    print("Scores are still computing; check back after feeding more frames.")
except VmafxError as err:
    print(f"Call {err.function} failed with {err.status}: {err.message}")
    if err.subject:
        print(f"Invalid subject: {err.subject}")
```

### Context teardown and callbacks

- **Retry-safe `close()`**: `Context.close()` conforms to
  [ADR-1336](../adr/1336-cuda-context-owned-resource-teardown.md). If
  `vmafx_context_destroy()` returns an error (such as `Status.E_BUSY`),
  `close()` raises `VmafxError` while retaining the context handle, allowing
  the caller to retry closing once in-flight tasks finish. A second `close()` on
  a successfully closed context is safely ignored as a no-op.
- **Python callback exceptions**: Any Python exception raised within a
  registered callback (such as `log_callback` in `Context` or
  `Library.context()`, or `on_complete` in `Context.window_submit()`) is
  trapped at the C FFI boundary, stored on the owning context, and re-raised on
  the calling Python thread. Python exceptions never unwind through foreign C
  stack frames.

## Wheels

Prebuilt wheels on PyPI are planned for the upcoming 0.1.0 release as part of
[issue 2318](https://github.com/VMAFx/vmafx/issues/2318) and
[ADR-3292](../adr/3292-release-line-restart-0x.md).

Prebuilt wheels are **not yet published** on PyPI. Users must install from
source as described in [Installation from source](#installation-from-source).

## See also

- [Research-2318](../research/2318-vmafx-python-package.md): Background research,
  source links, and design notes for the Python package and documentation.
- [Language bindings reference](../api/vmafx/bindings.md): Detailed C FFI
  coverage, wrapped vs. unwrapped functions, and asynchronous window scores.
- [VMAFx C API overview](../api/vmafx/index.md): The modernized VMAFx API design
  and header structure ([ADR-1852](../adr/1852-vmafx-api-redesign.md)).
- [Status codes](../api/vmafx/status-codes.md): Full reference of `VmafxStatus`
  codes and error subjects.
- [VMAF Python library](python.md): Legacy Netflix Python harness for model
  training, dataset evaluation, and format conversions.
