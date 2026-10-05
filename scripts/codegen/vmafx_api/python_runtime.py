# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixed parts of the generated Python binding (inserted verbatim by emit_python)."""

RUNTIME = '''class VmafxError(Exception):
    """A failed VMAFx call: status, message, the named subject and the engine's errno."""

    def __init__(
        self, function: str, status: int, message: str, subject: str, errno_value: int
    ) -> None:
        self.function = function
        self.status = status
        self.message = message
        self.subject = subject
        self.errno = errno_value
        name = Status(status).name if status in Status.__members__.values() else str(status)
        detail = f" [{subject}]" if subject else ""
        super().__init__(f"{function}: {name}: {message}{detail}")


class VmafxPending(VmafxError):
    """The requested score is not final yet (VMAFX_PENDING)."""


def _text(raw: bytes | None) -> str | None:
    return raw.decode() if raw is not None else None


def _check_layout() -> None:
    """Refuse a binding whose structs disagree with the definition's layout."""
    for struct, (size, offsets) in LAYOUT.items():
        if ctypes.sizeof(struct) != size:
            raise ImportError(f"{struct.__name__}: size {ctypes.sizeof(struct)} != {size}")
        for name, offset in offsets:
            actual = getattr(struct, name).offset
            if actual != offset:
                raise ImportError(f"{struct.__name__}.{name}: offset {actual} != {offset}")


def load(path: str | None = None) -> ctypes.CDLL:
    """Load the library from `path` or $VMAFX_LIBRARY. There is no search fallback."""
    chosen = path or os.environ.get("VMAFX_LIBRARY")
    if not chosen:
        raise OSError("set VMAFX_LIBRARY or pass the path of the VMAFx shared library")
    lib = ctypes.CDLL(chosen)
    for name, (restype, argtypes) in SIGNATURES.items():
        function = getattr(lib, name)
        function.restype = restype
        function.argtypes = argtypes
    return lib


def _raise(lib: ctypes.CDLL, status: int, error: ctypes.c_void_p, function: str) -> None:
    if status == Status.OK:
        return
    message = _text(lib.vmafx_error_message(error)) or ""
    subject = _text(lib.vmafx_error_subject(error)) or ""
    errno_value = lib.vmafx_error_errno(error)
    lib.vmafx_error_free(error)
    kind = VmafxPending if status == Status.PENDING else VmafxError
    raise kind(function, status, message, subject, errno_value)
'''

CONTEXT_CLASS = '''class Context:
    """A VmafxContext; close it with close() or a with-block."""

    def __init__(self, lib: ctypes.CDLL, config: ContextConfig | None = None) -> None:
        self._lib = lib
        raw = config.to_c() if config is not None else None
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        pointer = ctypes.byref(raw) if raw is not None else None
        status = lib.vmafx_context_create(pointer, ctypes.byref(handle), ctypes.byref(error))
        _raise(lib, status, error, "vmafx_context_create")
        self._handle = handle

    def __enter__(self) -> Context:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        """Destroy the context; on failure it stays open and close() may be retried."""
        if not self._handle:
            return
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_destroy(self._handle, ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_context_destroy")
        self._handle = ctypes.c_void_p()

    def libvmaf_handle(self) -> int:
        """The libvmaf VmafContext * bound to this context, for calls not yet migrated."""
        return self._lib.vmafx_context_libvmaf_handle(self._handle)
'''

LIBRARY_CLASS = '''class Library:
    """The loaded VMAFx library."""

    def __init__(self, path: str | None = None) -> None:
        self._lib = load(path)

    @property
    def raw(self) -> ctypes.CDLL:
        """The ctypes library, for calls the binding does not wrap."""
        return self._lib

    def context(self, config: ContextConfig | None = None) -> Context:
        """Create a context."""
        return Context(self._lib, config)
'''
