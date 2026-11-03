<!-- Copyright 2026 Lusoris -->
<!-- SPDX-License-Identifier: EUPL-1.2 -->
<!-- markdownlint-disable MD013 MD060 -->
# Research-2318: Python package bindings design and usage documentation

- **Status**: Active
- **Workstream**: [ADR-1852](../adr/1852-vmafx-api-redesign.md), [ADR-3292](../adr/3292-release-line-restart-0x.md)
- **Last updated**: 2026-11-03

## Question

How should the modernized VMAFx C API (`libvmafx.so.1`) be exposed, consumed, and documented in Python (`vmafx`), supporting zero-copy frame ingestion, explicit library loading, and safe error propagation without foreign C stack unwinding?

## Sources

- Single-source API definition: [`core/api/vmafx.toml`](https://github.com/VMAFx/vmafx/blob/master/core/api/vmafx.toml)
- Codegen script: `scripts/codegen/vmafx_api/` ([ADR-1852](../adr/1852-vmafx-api-redesign.md))
- C ABI headers: `core/include/vmafx/*.h`
- Python ctypes binding: `bindings/python/vmafx/_api.py` and `bindings/python/pyproject.toml`
- Prior research: [Research-2158](2158-vmafx-api-redesign.md) (section 1.1 / 1.2 on Python CLI reliance vs direct bindings)

## Findings

- Python buffer protocol: Zero-copy frame ingestion is supported for contiguous buffer-protocol objects (`memoryview.contiguous`) whose plane dimensions and strides match `FrameDesc` geometry (`_buffer_view()` and `_validate_one_plane()`). Non-contiguous buffers or invalid geometries are rejected with `ValueError`.
- Layout verification: `_check_layout()` runs at import time, validating the size and field offsets of the generated `LAYOUT` mapping against the `ctypes.sizeof` and field offsets of the ctypes structs, raising `ImportError` on any discrepancy before dynamic library loading.
- Library loading: `load()` requires either an explicit path argument or the `VMAFX_LIBRARY` environment variable. The binding does not implement an implicit path search fallback; if given a bare soname (such as `libvmafx.so.1`), resolution is handled by the platform dynamic linker using standard system search paths.
- Context and resource cleanup: Resource-owning classes (`Context`, `Model`, `ModelSet`, `Device`, `Frame`, `FramePool`, `FrameConverter`, `Options`, `ReportFile`, `Window`, `WindowClock`, `DnnSession`, `McpServer`) implement the context-manager protocol (`__enter__`/`__exit__`). `Library` represents the loaded library handle and does not implement `__enter__`/`__exit__`.
- Error and callback safety: Nonzero C status codes raise `VmafxError` or `VmafxPending`. In asynchronous tumbling window operations, `WindowClock.frame()` and `WindowClock.finish()` return `None` when a window span has not yet completed (`Status.PENDING`). Python callbacks (`log_callback` in `Context` or `Library.context()`, and `on_complete` in `Context.window_submit()`) intercept Python exceptions at the FFI boundary, storing them in `_CallbackState` to prevent foreign unwinding into C.

## Alternatives explored

- Implementing `__enter__`/`__exit__` on `Library`: `Library` has no `close()` method or native destroy handle since `ctypes.CDLL` handles are managed by Python's garbage collector and OS dlopen reference counts. Adding a no-op context manager was rejected in favor of keeping the context manager protocol restricted to actual resource-owning handles.
- Passing callbacks in dataclasses (`ContextConfig` / `WindowRequest`): C ABI structs require C callback function pointers and user data contexts. Instead of polluting frozen dataclasses with Python callables, Python callbacks are passed as keyword arguments to methods (`Library.context(log_callback=...)` and `Context.window_submit(on_complete=...)`), where they are wrapped in exception-trapping thunks.

## Open questions

- Wheel distribution: Prebuilt wheels on PyPI for Linux, macOS, and Windows are tracked under issue #2318 for the upcoming 0.1.0 release.

## Related

- ADRs: [ADR-1852](../adr/1852-vmafx-api-redesign.md), [ADR-3292](../adr/3292-release-line-restart-0x.md)
- PRs: #3273
- Issues: #2318
