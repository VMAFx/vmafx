# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixed parts of the generated Python binding (inserted verbatim by emit_python)."""

RUNTIME = '''class VmafxError(Exception):
    """A failed VMAFx call: status, message, the named subject and the engine's errno."""

    def __init__(
        self,
        function: str,
        status: int,
        message: str,
        subject: str,
        errno_value: int,
        subject_kind: int = 0,
    ) -> None:
        self.function = function
        self.status = status
        self.message = message
        self.subject = subject
        self.errno = errno_value
        self.subject_kind = subject_kind
        name = Status(status).name if status in Status.__members__.values() else str(status)
        detail = f" [{subject}]" if subject else ""
        super().__init__(f"{function}: {name}: {message}{detail}")


class VmafxPending(VmafxError):
    """The requested score is not final yet (VMAFX_PENDING)."""


class _PyBuffer(ctypes.Structure):
    """CPython Py_buffer header for buffer-protocol address extraction."""

    _fields_ = (
        ("buf", ctypes.c_void_p),
        ("obj", ctypes.py_object),
        ("len", ctypes.c_ssize_t),
        ("itemsize", ctypes.c_ssize_t),
        ("readonly", ctypes.c_int),
        ("ndim", ctypes.c_int),
        ("format", ctypes.c_char_p),
        ("shape", ctypes.POINTER(ctypes.c_ssize_t)),
        ("strides", ctypes.POINTER(ctypes.c_ssize_t)),
        ("suboffsets", ctypes.POINTER(ctypes.c_ssize_t)),
        ("internal", ctypes.c_void_p),
    )


_GET_BUFFER = ctypes.pythonapi.PyObject_GetBuffer
_GET_BUFFER.argtypes = (ctypes.py_object, ctypes.POINTER(_PyBuffer), ctypes.c_int)
_GET_BUFFER.restype = ctypes.c_int
_RELEASE_BUFFER = ctypes.pythonapi.PyBuffer_Release
_RELEASE_BUFFER.argtypes = (ctypes.POINTER(_PyBuffer),)
_RELEASE_BUFFER.restype = None


def _buffer_view(exporter: object, *, writable: bool, label: str) -> tuple[memoryview, int]:
    """Pin `exporter` as a contiguous memoryview and return `(view, address)`."""
    mv = memoryview(exporter)  # type: ignore[arg-type]  # buffer protocol checked at runtime
    if not mv.contiguous:
        mv.release()
        raise ValueError(f"{label} must be contiguous")
    if writable and mv.readonly:
        mv.release()
        raise ValueError(f"{label} must be writable")
    raw = _PyBuffer()
    if _GET_BUFFER(mv, ctypes.byref(raw), 0) != 0:
        mv.release()
        raise ValueError(f"cannot read buffer address of {label}")
    address = int(raw.buf or 0)
    _RELEASE_BUFFER(ctypes.byref(raw))
    return mv, address


class _CallbackState:
    """Captures the first Python exception raised in a C callback so it never crosses FFI."""

    def __init__(self) -> None:
        self.exc: BaseException | None = None

    def record(self, exc: BaseException) -> None:
        if self.exc is None:
            self.exc = exc

    def reraise(self) -> None:
        if self.exc is not None:
            exc = self.exc
            self.exc = None
            raise exc


def _make_log_thunk(
    callback: Callable[[int, str], None] | None,
) -> tuple[object, _CallbackState | None]:
    """Wrap a Python log callable into a VmafxLogCallback thunk that traps exceptions."""
    if callback is None:
        return VmafxLogCallback(), None
    state = _CallbackState()

    def _thunk(level: int, message: bytes | None, _user: int | None) -> None:
        try:
            callback(int(level), message.decode(errors="replace") if message else "")
        except BaseException as exc:  # FFI boundary must not unwind through C
            state.record(exc)

    return VmafxLogCallback(_thunk), state


_NEXT_HOST_TOKEN = 1
_PINNED_HOSTS: dict[int, tuple[tuple[object, ...], tuple[memoryview, ...], object, object]] = {}
_PIX_FMT_YUV420P = 1
_PIX_FMT_YUV422P = 2
_PIX_FMT_YUV444P = 3
_PIX_FMT_YUV400P = 4
_BPC_MIN = 8
_BPC_MAX = 16
_MAX_CLOSE_ATTEMPTS = 2


def _release_pinned_host(token: int) -> None:
    """Drop pinned buffer references when libvmafx releases a wrapped host frame."""
    entry = _PINNED_HOSTS.pop(token, None)
    if entry is None:
        return
    _, views, _, callback = entry
    for view in views:
        view.release()
    if callable(callback):
        callback()


def _plane_extents(pix_fmt: int, w: int, h: int) -> tuple[tuple[int, int], ...]:
    """Plane `(width, height)` pairs in samples for `pix_fmt` (`vmaf_picture_plane_extents`)."""
    if w <= 0 or h <= 0:
        raise ValueError(f"frame dimensions must be positive, got {w}x{h}")
    if pix_fmt == _PIX_FMT_YUV400P:
        return ((w, h),)
    if pix_fmt == _PIX_FMT_YUV420P:
        return ((w, h), ((w + 1) // 2, (h + 1) // 2), ((w + 1) // 2, (h + 1) // 2))
    if pix_fmt == _PIX_FMT_YUV422P:
        return ((w, h), ((w + 1) // 2, h), ((w + 1) // 2, h))
    if pix_fmt == _PIX_FMT_YUV444P:
        return ((w, h), (w, h), (w, h))
    raise ValueError(f"unsupported pixel format {pix_fmt}")


def _validate_one_plane(
    idx: int,
    pw: int,
    ph: int,
    sample_bytes: int,
    exporter: object,
    stride_override: int | None,
) -> tuple[memoryview, int, int]:
    """Validate one buffer-protocol plane against its `(pw, ph)` geometry."""
    min_stride = pw * sample_bytes
    stride = int(stride_override) if stride_override is not None else min_stride
    if stride < min_stride:
        raise ValueError(f"plane[{idx}] stride {stride} < minimum row bytes {min_stride}")
    mv, addr = _buffer_view(exporter, writable=False, label=f"plane[{idx}]")
    required = (ph - 1) * stride + min_stride
    nbytes = mv.nbytes
    if nbytes < required:
        mv.release()
        raise ValueError(f"plane[{idx}] buffer length {nbytes} < required {required} bytes")
    return mv, addr, stride


def _validate_host_planes(
    desc: FrameDesc,
    planes: Sequence[object],
    strides: Sequence[int] | None,
) -> tuple[tuple[memoryview, ...], tuple[int, ...], tuple[int, ...]]:
    """Validate buffer-protocol planes against `desc` geometry before calling C."""
    if not (_BPC_MIN <= desc.bpc <= _BPC_MAX):
        raise ValueError(f"bpc {desc.bpc} outside 8..16")
    extents = _plane_extents(desc.pix_fmt, desc.w, desc.h)
    if len(planes) != len(extents):
        raise ValueError(f"plane[{len(planes)}] count {len(planes)} != expected {len(extents)}")
    resolved_in = tuple(strides) if strides is not None else (None,) * len(extents)
    if len(resolved_in) != len(extents):
        raise ValueError(f"plane stride count {len(resolved_in)} != expected {len(extents)}")
    sample_bytes = 1 + int(desc.bpc > _BPC_MIN)
    views: list[memoryview] = []
    addrs: list[int] = []
    resolved_strides: list[int] = []
    try:
        for idx, ((pw, ph), exporter, override) in enumerate(
            zip(extents, planes, resolved_in, strict=True)
        ):
            mv, addr, stride = _validate_one_plane(idx, pw, ph, sample_bytes, exporter, override)
            views.append(mv)
            addrs.append(addr)
            resolved_strides.append(stride)
    except BaseException:
        for view in views:
            view.release()
        raise
    return tuple(views), tuple(addrs), tuple(resolved_strides)


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


def _raise(
    lib: ctypes.CDLL,
    status: int,
    error: ctypes.c_void_p,
    function: str,
    log_state: _CallbackState | None = None,
) -> None:
    if log_state is not None:
        log_state.reraise()
    if status == Status.OK:
        return
    err_status = int(lib.vmafx_error_status(error)) if error else status
    message = _text(lib.vmafx_error_message(error)) or ""
    subject = _text(lib.vmafx_error_subject(error)) or ""
    errno_value = int(lib.vmafx_error_errno(error))
    subject_kind = int(lib.vmafx_error_subject_kind(error))
    err_fn = _text(lib.vmafx_error_function(error)) or function
    lib.vmafx_error_free(error)
    kind = VmafxPending if err_status == Status.PENDING else VmafxError
    raise kind(err_fn, err_status, message, subject, errno_value, subject_kind)
'''

HANDLE_CLASSES = '''def _raw_device_desc(desc: DeviceDesc | VmafxDeviceDesc | None) -> VmafxDeviceDesc | None:
    if desc is None:
        return None
    if isinstance(desc, VmafxDeviceDesc):
        return desc
    raw = VmafxDeviceDesc()
    raw.struct_size = ctypes.sizeof(raw)
    raw.backend = desc.backend
    raw.index = desc.index
    raw.flags = desc.flags
    for idx, value in enumerate(desc.external[:2]):
        raw.external[idx] = value
    return raw


def _raw_model_config(
    config: ModelConfig | VmafxModelConfig | None,
    log_callback: Callable[[int, str], None] | None,
) -> tuple[VmafxModelConfig | None, bytes | None, object, _CallbackState | None]:
    thunk, state = _make_log_thunk(log_callback)
    if config is None and log_callback is None:
        return None, None, thunk, state
    if isinstance(config, VmafxModelConfig):
        if log_callback is not None:
            config.log_callback = thunk
        return config, None, thunk, state
    raw = VmafxModelConfig()
    raw.struct_size = ctypes.sizeof(raw)
    name_bytes = config.name.encode() if config is not None and config.name else None
    raw.name = name_bytes
    raw.flags = config.flags if config is not None else 0
    raw.log_level = config.log_level if config is not None else 0
    if log_callback is not None:
        raw.log_callback = thunk
    return raw, name_bytes, thunk, state


class Options:
    """A `VmafxOptions` key-value list (`vmafx_options_set` / `vmafx_options_free`)."""

    def __init__(self, lib: ctypes.CDLL, values: Mapping[str, str] | None = None) -> None:
        self._lib = lib
        self._handle = ctypes.c_void_p()
        if values:
            for key, value in values.items():
                self.set(key, value)

    def __enter__(self) -> Options:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def set(self, key: str, value: str) -> None:
        """Append or replace `key=value` (`vmafx_options_set`)."""
        error = ctypes.c_void_p()
        status = self._lib.vmafx_options_set(
            ctypes.byref(self._handle), key.encode(), value.encode(), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_options_set")

    def close(self) -> None:
        """Release the options list (`vmafx_options_free`)."""
        if self._handle:
            self._lib.vmafx_options_free(self._handle)
            self._handle = ctypes.c_void_p()


class Device:
    """A reference-counted `VmafxDevice` (`vmafx_device_*`)."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        desc: DeviceDesc | VmafxDeviceDesc | None = None,
        *,
        _handle: ctypes.c_void_p | None = None,
    ) -> None:
        self._lib = lib
        if _handle is not None:
            self._handle = _handle
            return
        raw = _raw_device_desc(desc)
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        pointer = ctypes.byref(raw) if raw is not None else None
        status = lib.vmafx_device_create(pointer, ctypes.byref(handle), ctypes.byref(error))
        _raise(lib, status, error, "vmafx_device_create")
        self._handle = handle

    def __enter__(self) -> Device:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def ref(self) -> Device:
        """Take an additional reference (`vmafx_device_ref`)."""
        ptr = self._lib.vmafx_device_ref(self._handle)
        return Device(self._lib, _handle=ctypes.c_void_p(ptr))

    def unref(self) -> None:
        """Drop this reference (`vmafx_device_unref`)."""
        if self._handle:
            self._lib.vmafx_device_unref(self._handle)
            self._handle = ctypes.c_void_p()

    def close(self) -> None:
        """Alias for `unref()`."""
        self.unref()

    def backend(self) -> int:
        """The device's `VmafxBackend` (`vmafx_device_backend`)."""
        return int(self._lib.vmafx_device_backend(self._handle))

    def describe(self) -> DeviceInfo:
        """Describe the open device (`vmafx_device_describe`)."""
        out = VmafxDeviceInfo()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_device_describe(
            self._handle, ctypes.byref(out), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_device_describe")
        return DeviceInfo.from_c(out)

    def profile(self) -> str:
        """JSON profile of device timing counters (`vmafx_device_profile`)."""
        out = ctypes.c_char_p()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_device_profile(
            self._handle, ctypes.byref(out), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_device_profile")
        return _text(out.value) or ""


class Frame:
    """A reference-counted `VmafxFrame` (`vmafx_frame_*`)."""

    def __init__(self, lib: ctypes.CDLL, handle: ctypes.c_void_p) -> None:
        self._lib = lib
        self._handle = handle

    @classmethod
    def create_host(cls, lib: ctypes.CDLL, desc: FrameDesc, device: Device | None = None) -> Frame:
        """Allocate a host frame (`vmafx_frame_create_host`)."""
        raw = desc.to_c()
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        dev_ptr = device._handle if device is not None else None
        status = lib.vmafx_frame_create_host(
            dev_ptr, ctypes.byref(raw), ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_frame_create_host")
        return cls(lib, handle)

    @classmethod
    def wrap_host(
        cls,
        lib: ctypes.CDLL,
        desc: FrameDesc,
        planes: Sequence[object],
        strides: Sequence[int] | None = None,
        *,
        device: Device | None = None,
        on_release: Callable[[], None] | None = None,
    ) -> Frame:
        """Wrap buffer-protocol planes without copying (`vmafx_frame_wrap_host`)."""
        global _NEXT_HOST_TOKEN  # noqa: PLW0603 -- monotonic key for pinned buffer registry
        views, addrs, resolved_strides = _validate_host_planes(desc, planes, strides)
        token = _NEXT_HOST_TOKEN
        _NEXT_HOST_TOKEN += 1

        def _c_release(user: int | None) -> None:
            _release_pinned_host(int(user or token))

        thunk = VmafxFrameReleaseCallback(_c_release)
        _PINNED_HOSTS[token] = (tuple(planes), views, thunk, on_release)
        raw_desc = desc.to_c()
        raw_planes = VmafxHostPlanes()
        raw_planes.struct_size = ctypes.sizeof(raw_planes)
        for idx, (addr, stride) in enumerate(zip(addrs, resolved_strides, strict=True)):
            raw_planes.data[idx] = addr
            raw_planes.stride[idx] = stride
        raw_planes.release = thunk
        raw_planes.user = token
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        dev_ptr = device._handle if device is not None else None
        status = lib.vmafx_frame_wrap_host(
            dev_ptr,
            ctypes.byref(raw_desc),
            ctypes.byref(raw_planes),
            ctypes.byref(handle),
            ctypes.byref(error),
        )
        if status != Status.OK:
            _release_pinned_host(token)
            _raise(lib, status, error, "vmafx_frame_wrap_host")
        return cls(lib, handle)

    def __enter__(self) -> Frame:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def planes(self) -> FramePlanes:
        """Plane geometry of a host frame (`vmafx_frame_planes`)."""
        out = VmafxFramePlanes()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_frame_planes(self._handle, ctypes.byref(out), ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_frame_planes")
        return FramePlanes.from_c(out)

    def plane_buffers(self) -> tuple[memoryview, ...]:
        """Writable `memoryview`s over each plane of a host frame (`vmafx_frame_planes`)."""
        out = VmafxFramePlanes()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_frame_planes(self._handle, ctypes.byref(out), ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_frame_planes")
        views: list[memoryview] = []
        for idx in range(int(out.n_planes)):
            nbytes = int(out.h[idx]) * int(out.stride[idx])
            buf = (ctypes.c_char * nbytes).from_address(int(out.data[idx]))
            views.append(memoryview(buf).cast("B"))
        return tuple(views)

    def ref(self) -> Frame:
        """Take an additional reference (`vmafx_frame_ref`)."""
        ptr = self._lib.vmafx_frame_ref(self._handle)
        return Frame(self._lib, ctypes.c_void_p(ptr))

    def unref(self) -> None:
        """Drop this reference (`vmafx_frame_unref`)."""
        if self._handle:
            self._lib.vmafx_frame_unref(self._handle)
            self._handle = ctypes.c_void_p()

    def close(self) -> None:
        """Alias for `unref()`."""
        self.unref()

    def release_fence(self, kind: int) -> Fence:
        """Export a fence signalled when the frame is released (`vmafx_frame_release_fence`)."""
        out = VmafxFence()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_frame_release_fence(
            self._handle, kind, ctypes.byref(out), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_frame_release_fence")
        return Fence.from_c(out)

    def signal_on_release(self, signal: Fence | VmafxFence) -> None:
        """Signal caller fence when the frame is released (`vmafx_frame_signal_on_release`)."""
        raw = signal if isinstance(signal, VmafxFence) else signal.to_c()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_frame_signal_on_release(
            self._handle, ctypes.byref(raw), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_frame_signal_on_release")


class FramePool:
    """A reusable `VmafxFramePool` (`vmafx_frame_pool_*`)."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        desc: FrameDesc,
        count: int,
        device: Device | None = None,
    ) -> None:
        self._lib = lib
        raw = desc.to_c()
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        dev_ptr = device._handle if device is not None else None
        status = lib.vmafx_frame_pool_create(
            dev_ptr, ctypes.byref(raw), count, ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_frame_pool_create")
        self._handle = handle

    def __enter__(self) -> FramePool:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def acquire(self) -> Frame:
        """Acquire a frame from the pool (`vmafx_frame_pool_acquire`)."""
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_frame_pool_acquire(
            self._handle, ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_frame_pool_acquire")
        return Frame(self._lib, handle)

    def close(self) -> None:
        """Destroy the pool (`vmafx_frame_pool_destroy`)."""
        if self._handle:
            self._lib.vmafx_frame_pool_destroy(self._handle)
            self._handle = ctypes.c_void_p()


class FrameConverter:
    """A reusable `VmafxFrameConverter` (`vmafx_frame_converter_*`)."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        desc: ConvertDesc | VmafxConvertDesc | None = None,
        *,
        _handle: ctypes.c_void_p | None = None,
    ) -> None:
        self._lib = lib
        if _handle is not None:
            self._handle = _handle
            return
        raw = (
            desc
            if isinstance(desc, VmafxConvertDesc)
            else (desc.to_c() if desc is not None else VmafxConvertDesc())
        )
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        status = lib.vmafx_frame_converter_create(
            ctypes.byref(raw), ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_frame_converter_create")
        self._handle = handle

    def __enter__(self) -> FrameConverter:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            with contextlib.suppress(VmafxError):
                self.close()

    def convert(self, src: Frame) -> Frame:
        """Convert `src` into a newly allocated host frame (`vmafx_frame_convert`)."""
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_frame_convert(
            self._handle, src._handle, ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_frame_convert")
        return Frame(self._lib, handle)

    def close(self) -> None:
        """Destroy the converter (`vmafx_frame_converter_destroy`)."""
        if not self._handle:
            return
        error = ctypes.c_void_p()
        status = self._lib.vmafx_frame_converter_destroy(self._handle, ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_frame_converter_destroy")
        self._handle = ctypes.c_void_p()


class Model:
    """A reference-counted `VmafxModel` (`vmafx_model_*`)."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        handle: ctypes.c_void_p,
        log_ref: object = None,
    ) -> None:
        self._lib = lib
        self._handle = handle
        self._log_ref = log_ref

    @classmethod
    def load(
        cls,
        lib: ctypes.CDLL,
        version: str | None = None,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> Model:
        """Load a built-in model (`vmafx_model_load`)."""
        raw_cfg, name_b, thunk, state = _raw_model_config(config, log_callback)
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        cfg_ptr = ctypes.byref(raw_cfg) if raw_cfg is not None else None
        ver_b = version.encode() if version is not None else None
        status = lib.vmafx_model_load(cfg_ptr, ver_b, ctypes.byref(handle), ctypes.byref(error))
        _raise(lib, status, error, "vmafx_model_load", state)
        return cls(lib, handle, (thunk, state, name_b))

    @classmethod
    def load_file(
        cls,
        lib: ctypes.CDLL,
        path: str,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> Model:
        """Load a model from `path` (`vmafx_model_load_file`)."""
        raw_cfg, name_b, thunk, state = _raw_model_config(config, log_callback)
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        cfg_ptr = ctypes.byref(raw_cfg) if raw_cfg is not None else None
        status = lib.vmafx_model_load_file(
            cfg_ptr, path.encode(), ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_model_load_file", state)
        return cls(lib, handle, (thunk, state, name_b))

    @classmethod
    def load_spec(
        cls,
        lib: ctypes.CDLL,
        spec: str,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> Model:
        """Load a model from a CLI `key=value[:key=value...]` spec (`vmafx_model_load_spec`)."""
        raw_cfg, name_b, thunk, state = _raw_model_config(config, log_callback)
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        cfg_ptr = ctypes.byref(raw_cfg) if raw_cfg is not None else None
        status = lib.vmafx_model_load_spec(
            cfg_ptr, spec.encode(), ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_model_load_spec", state)
        return cls(lib, handle, (thunk, state, name_b))

    def __enter__(self) -> Model:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def override_feature(self, extractor: str, options: Options) -> None:
        """Attach per-extractor options to this model (`vmafx_model_override_feature`)."""
        error = ctypes.c_void_p()
        status = self._lib.vmafx_model_override_feature(
            self._handle, extractor.encode(), options._handle, ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_model_override_feature")

    def ref(self) -> Model:
        """Take an additional reference (`vmafx_model_ref`)."""
        ptr = self._lib.vmafx_model_ref(self._handle)
        return Model(self._lib, ctypes.c_void_p(ptr), self._log_ref)

    def unref(self) -> None:
        """Drop this reference (`vmafx_model_unref`)."""
        if self._handle:
            self._lib.vmafx_model_unref(self._handle)
            self._handle = ctypes.c_void_p()

    def close(self) -> None:
        """Alias for `unref()`."""
        self.unref()

    def name(self) -> str | None:
        """The model's name (`vmafx_model_name`)."""
        return _text(self._lib.vmafx_model_name(self._handle))

    def feature_count(self) -> int:
        """Number of features the model requires (`vmafx_model_feature_count`)."""
        return int(self._lib.vmafx_model_feature_count(self._handle))

    def feature_name(self, index: int) -> str | None:
        """The `index`-th required feature name (`vmafx_model_feature_name`)."""
        return _text(self._lib.vmafx_model_feature_name(self._handle, index))

    def hash(self) -> str | None:
        """SHA-256 hex digest of the loaded model (`vmafx_model_hash`)."""
        return _text(self._lib.vmafx_model_hash(self._handle))


class ModelSet:
    """A reference-counted `VmafxModelSet` (`vmafx_model_set_*`)."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        handle: ctypes.c_void_p,
        log_ref: object = None,
    ) -> None:
        self._lib = lib
        self._handle = handle
        self._log_ref = log_ref

    @classmethod
    def load(
        cls,
        lib: ctypes.CDLL,
        version: str,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> ModelSet:
        """Load a built-in model collection (`vmafx_model_set_load`)."""
        raw_cfg, name_b, thunk, state = _raw_model_config(config, log_callback)
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        cfg_ptr = ctypes.byref(raw_cfg) if raw_cfg is not None else None
        status = lib.vmafx_model_set_load(
            cfg_ptr, version.encode(), ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_model_set_load", state)
        return cls(lib, handle, (thunk, state, name_b))

    @classmethod
    def load_file(
        cls,
        lib: ctypes.CDLL,
        path: str,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> ModelSet:
        """Load a model collection from `path` (`vmafx_model_set_load_file`)."""
        raw_cfg, name_b, thunk, state = _raw_model_config(config, log_callback)
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        cfg_ptr = ctypes.byref(raw_cfg) if raw_cfg is not None else None
        status = lib.vmafx_model_set_load_file(
            cfg_ptr, path.encode(), ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_model_set_load_file", state)
        return cls(lib, handle, (thunk, state, name_b))

    def __enter__(self) -> ModelSet:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def override_feature(self, extractor: str, options: Options) -> None:
        """Attach per-extractor options to this model set (`vmafx_model_set_override_feature`)."""
        error = ctypes.c_void_p()
        status = self._lib.vmafx_model_set_override_feature(
            self._handle, extractor.encode(), options._handle, ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_model_set_override_feature")

    def ref(self) -> ModelSet:
        """Take an additional reference (`vmafx_model_set_ref`)."""
        ptr = self._lib.vmafx_model_set_ref(self._handle)
        return ModelSet(self._lib, ctypes.c_void_p(ptr), self._log_ref)

    def unref(self) -> None:
        """Drop this reference (`vmafx_model_set_unref`)."""
        if self._handle:
            self._lib.vmafx_model_set_unref(self._handle)
            self._handle = ctypes.c_void_p()

    def close(self) -> None:
        """Alias for `unref()`."""
        self.unref()

    def lead(self) -> Model | None:
        """The set's lead model with its own reference (`vmafx_model_set_lead`)."""
        borrowed = self._lib.vmafx_model_set_lead(self._handle)
        if not borrowed:
            return None
        owned = self._lib.vmafx_model_ref(ctypes.c_void_p(borrowed))
        return Model(self._lib, ctypes.c_void_p(owned))

    def size(self) -> int:
        """Number of models in the set (`vmafx_model_set_size`)."""
        return int(self._lib.vmafx_model_set_size(self._handle))

    def hash(self) -> str | None:
        """SHA-256 hex digest of the loaded model set (`vmafx_model_set_hash`)."""
        return _text(self._lib.vmafx_model_set_hash(self._handle))


class ReportFile:
    """A parsed JSON or XML report (`vmafx_report_*`)."""

    def __init__(self, lib: ctypes.CDLL, path: str) -> None:
        self._lib = lib
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        status = lib.vmafx_report_open(path.encode(), ctypes.byref(handle), ctypes.byref(error))
        _raise(lib, status, error, "vmafx_report_open")
        self._handle = handle

    def __enter__(self) -> ReportFile:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def field(self, path: str) -> str | None:
        """Read a dotted field from the report (`vmafx_report_field`)."""
        return _text(self._lib.vmafx_report_field(self._handle, path.encode()))

    def verify(self, rerun: ReportFile) -> None:
        """Verify that `rerun` reproduces this report (`vmafx_report_verify`)."""
        error = ctypes.c_void_p()
        status = self._lib.vmafx_report_verify(self._handle, rerun._handle, ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_report_verify")

    def close(self) -> None:
        """Close the parsed report (`vmafx_report_close`)."""
        if self._handle:
            self._lib.vmafx_report_close(self._handle)
            self._handle = ctypes.c_void_p()


class Window:
    """An asynchronous pooled-score request (`vmafx_window_*`)."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        handle: ctypes.c_void_p,
        callback_ref: object,
        state: _CallbackState,
        context_state: _CallbackState,
    ) -> None:
        self._lib = lib
        self._handle = handle
        self._callback_ref = callback_ref
        self._state = state
        self._context_state = context_state

    def __enter__(self) -> Window:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def _reraise(self) -> None:
        self._state.reraise()
        self._context_state.reraise()

    def poll(self) -> WindowResult:
        """Poll without blocking (`vmafx_window_poll`)."""
        out = VmafxWindowResult()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_window_poll(self._handle, ctypes.byref(out), ctypes.byref(error))
        self._reraise()
        _raise(self._lib, status, error, "vmafx_window_poll")
        return WindowResult.from_c(out)

    def wait(self, timeout_ns: int = 0xFFFF_FFFF_FFFF_FFFF) -> WindowResult:
        """Wait up to `timeout_ns` nanoseconds (`vmafx_window_wait`)."""
        out = VmafxWindowResult()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_window_wait(
            self._handle, timeout_ns, ctypes.byref(out), ctypes.byref(error)
        )
        self._reraise()
        _raise(self._lib, status, error, "vmafx_window_wait")
        return WindowResult.from_c(out)

    def release(self) -> None:
        """Release the window handle (`vmafx_window_release`)."""
        if self._handle:
            self._lib.vmafx_window_release(self._handle)
            self._handle = ctypes.c_void_p()

    def close(self) -> None:
        """Alias for `release()`."""
        self.release()


class WindowClock:
    """A tumbling window clock (`vmafx_window_clock_*`)."""

    def __init__(
        self, lib: ctypes.CDLL, config: WindowClockConfig | VmafxWindowClockConfig
    ) -> None:
        self._lib = lib
        raw = config if isinstance(config, VmafxWindowClockConfig) else config.to_c()
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        status = lib.vmafx_window_clock_create(
            ctypes.byref(raw), ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_window_clock_create")
        self._handle = handle

    def __enter__(self) -> WindowClock:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def frame(self, index: int, pts_ns: int) -> WindowSpan | None:
        """Advance the clock with frame `index` at `pts_ns` (`vmafx_window_clock_frame`)."""
        out = VmafxWindowSpan()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_window_clock_frame(
            self._handle, index, pts_ns, ctypes.byref(out), ctypes.byref(error)
        )
        if status == Status.PENDING:
            return None
        _raise(self._lib, status, error, "vmafx_window_clock_frame")
        return WindowSpan.from_c(out)

    def finish(self) -> WindowSpan | None:
        """Close the trailing window at end of stream (`vmafx_window_clock_finish`)."""
        out = VmafxWindowSpan()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_window_clock_finish(
            self._handle, ctypes.byref(out), ctypes.byref(error)
        )
        if status == Status.PENDING:
            return None
        _raise(self._lib, status, error, "vmafx_window_clock_finish")
        return WindowSpan.from_c(out)

    def close(self) -> None:
        """Destroy the clock (`vmafx_window_clock_destroy`)."""
        if self._handle:
            self._lib.vmafx_window_clock_destroy(self._handle)
            self._handle = ctypes.c_void_p()


class DnnSession:
    """An ONNX Runtime session (`vmafx_dnn_session_*`)."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        onnx_path: str | None = None,
        config: DnnConfig | VmafxDnnConfig | None = None,
        *,
        _handle: ctypes.c_void_p | None = None,
    ) -> None:
        self._lib = lib
        if _handle is not None:
            self._handle = _handle
            return
        if onnx_path is None:
            raise ValueError("onnx_path is required to open a DnnSession")
        raw = config if isinstance(config, VmafxDnnConfig) or config is None else config.to_c()
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        pointer = ctypes.byref(raw) if raw is not None else None
        status = lib.vmafx_dnn_session_open(
            onnx_path.encode(), pointer, ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_dnn_session_open")
        self._handle = handle

    def __enter__(self) -> DnnSession:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def run_luma8(
        self, src: object, in_stride: int, w: int, h: int, dst: object, out_stride: int
    ) -> None:
        """Run on an 8-bit luma plane (`vmafx_dnn_session_run_luma8`)."""
        in_mv, in_addr = _buffer_view(src, writable=False, label="src")
        try:
            out_mv, out_addr = _buffer_view(dst, writable=True, label="dst")
        except BaseException:
            in_mv.release()
            raise
        try:
            error = ctypes.c_void_p()
            status = self._lib.vmafx_dnn_session_run_luma8(
                self._handle,
                in_addr,
                in_stride,
                w,
                h,
                out_addr,
                out_stride,
                ctypes.byref(error),
            )
            _raise(self._lib, status, error, "vmafx_dnn_session_run_luma8")
        finally:
            out_mv.release()
            in_mv.release()

    def run_plane16(
        self,
        src: object,
        in_stride: int,
        w: int,
        h: int,
        bpc: int,
        dst: object,
        out_stride: int,
    ) -> None:
        """Run on a 10/12/16-bit luma plane (`vmafx_dnn_session_run_plane16`)."""
        in_mv, in_addr = _buffer_view(src, writable=False, label="src")
        try:
            out_mv, out_addr = _buffer_view(dst, writable=True, label="dst")
        except BaseException:
            in_mv.release()
            raise
        try:
            error = ctypes.c_void_p()
            status = self._lib.vmafx_dnn_session_run_plane16(
                self._handle,
                in_addr,
                in_stride,
                w,
                h,
                bpc,
                out_addr,
                out_stride,
                ctypes.byref(error),
            )
            _raise(self._lib, status, error, "vmafx_dnn_session_run_plane16")
        finally:
            out_mv.release()
            in_mv.release()

    def run(
        self,
        inputs: Sequence[tuple[str, object, Sequence[int]]],
        outputs: Sequence[tuple[str, object]],
    ) -> tuple[int, ...]:
        """Run named float tensors (`vmafx_dnn_session_run`); returns floats written per output."""
        views: list[memoryview] = []
        keep: list[object] = []
        try:
            c_inputs = (VmafxDnnInput * len(inputs))()
            for idx, (name, data, shape) in enumerate(inputs):
                mv, addr = _buffer_view(data, writable=False, label=f"inputs[{idx}]")
                views.append(mv)
                name_b = name.encode()
                c_shape = (ctypes.c_int64 * len(shape))(*shape)
                keep.extend((name_b, c_shape))
                c_inputs[idx].name = name_b
                c_inputs[idx].data = addr
                c_inputs[idx].shape = ctypes.addressof(c_shape)
                c_inputs[idx].rank = len(shape)
            c_outputs = (VmafxDnnOutput * len(outputs))()
            for idx, (name, data) in enumerate(outputs):
                mv, addr = _buffer_view(data, writable=True, label=f"outputs[{idx}]")
                views.append(mv)
                name_b = name.encode()
                keep.append(name_b)
                c_outputs[idx].name = name_b
                c_outputs[idx].data = addr
                c_outputs[idx].capacity = mv.nbytes // ctypes.sizeof(ctypes.c_float)
                c_outputs[idx].written = 0
            error = ctypes.c_void_p()
            in_ptr = ctypes.cast(c_inputs, ctypes.POINTER(VmafxDnnInput)) if inputs else None
            out_ptr = ctypes.cast(c_outputs, ctypes.POINTER(VmafxDnnOutput)) if outputs else None
            status = self._lib.vmafx_dnn_session_run(
                self._handle,
                in_ptr,
                len(inputs),
                out_ptr,
                len(outputs),
                ctypes.byref(error),
            )
            _raise(self._lib, status, error, "vmafx_dnn_session_run")
            return tuple(int(c_outputs[idx].written) for idx in range(len(outputs)))
        finally:
            for view in views:
                view.release()

    def runtime_device(self) -> str | None:
        """Execution provider attached to the session (`vmafx_dnn_session_runtime_device`)."""
        return _text(self._lib.vmafx_dnn_session_runtime_device(self._handle))

    def close(self) -> None:
        """Close the session (`vmafx_dnn_session_close`)."""
        if self._handle:
            self._lib.vmafx_dnn_session_close(self._handle)
            self._handle = ctypes.c_void_p()


class McpServer:
    """An embedded MCP server (`vmafx_mcp_*`)."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        context: Context | None = None,
        config: McpConfig | VmafxMcpConfig | None = None,
        *,
        _handle: ctypes.c_void_p | None = None,
    ) -> None:
        self._lib = lib
        self._ua_bytes: bytes | None = None
        if _handle is not None:
            self._handle = _handle
            return
        raw: VmafxMcpConfig | None = None
        ua_bytes: bytes | None = None
        if isinstance(config, VmafxMcpConfig):
            raw = config
        elif config is not None:
            raw = VmafxMcpConfig()
            raw.struct_size = ctypes.sizeof(raw)
            raw.queue_depth = config.queue_depth
            raw.max_drain_per_frame = config.max_drain_per_frame
            ua_bytes = config.user_agent.encode() if config.user_agent else None
            raw.user_agent = ua_bytes
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        ctx_ptr = context._handle if context is not None else None
        cfg_ptr = ctypes.byref(raw) if raw is not None else None
        status = lib.vmafx_mcp_server_create(
            ctx_ptr, cfg_ptr, ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(lib, status, error, "vmafx_mcp_server_create")
        self._handle = handle
        self._ua_bytes = ua_bytes

    def __enter__(self) -> McpServer:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_handle", None):
            self.close()

    def start_sse(self, config: McpSseConfig | VmafxMcpSseConfig | None = None) -> int:
        """Start the HTTP/SSE transport (`vmafx_mcp_start_sse`); returns bound port."""
        raw: VmafxMcpSseConfig | None = None
        path_b: bytes | None = None
        if isinstance(config, VmafxMcpSseConfig):
            raw = config
        elif config is not None:
            raw = VmafxMcpSseConfig()
            raw.struct_size = ctypes.sizeof(raw)
            raw.port = config.port
            path_b = config.path.encode() if config.path else None
            raw.path = path_b
        port = ctypes.c_uint32()
        error = ctypes.c_void_p()
        cfg_ptr = ctypes.byref(raw) if raw is not None else None
        status = self._lib.vmafx_mcp_start_sse(
            self._handle, cfg_ptr, ctypes.byref(port), ctypes.byref(error)
        )
        _ = path_b
        _raise(self._lib, status, error, "vmafx_mcp_start_sse")
        return int(port.value)

    def start_uds(self, config: str | McpUdsConfig | VmafxMcpUdsConfig) -> None:
        """Start the Unix-domain socket transport (`vmafx_mcp_start_uds`)."""
        path_b: bytes | None = None
        if isinstance(config, VmafxMcpUdsConfig):
            raw = config
        else:
            raw = VmafxMcpUdsConfig()
            raw.struct_size = ctypes.sizeof(raw)
            path_str = config if isinstance(config, str) else config.path
            path_b = path_str.encode() if path_str else None
            raw.path = path_b
        error = ctypes.c_void_p()
        status = self._lib.vmafx_mcp_start_uds(self._handle, ctypes.byref(raw), ctypes.byref(error))
        _ = path_b
        _raise(self._lib, status, error, "vmafx_mcp_start_uds")

    def start_stdio(self, config: McpStdioConfig | VmafxMcpStdioConfig | None = None) -> None:
        """Start the stdio transport (`vmafx_mcp_start_stdio`)."""
        raw = config if isinstance(config, VmafxMcpStdioConfig) or config is None else config.to_c()
        error = ctypes.c_void_p()
        cfg_ptr = ctypes.byref(raw) if raw is not None else None
        status = self._lib.vmafx_mcp_start_stdio(self._handle, cfg_ptr, ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_mcp_start_stdio")

    def stop(self) -> None:
        """Stop all running transports (`vmafx_mcp_stop`)."""
        error = ctypes.c_void_p()
        status = self._lib.vmafx_mcp_stop(self._handle, ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_mcp_stop")

    def close(self) -> None:
        """Destroy the server (`vmafx_mcp_server_destroy`)."""
        if self._handle:
            self._lib.vmafx_mcp_server_destroy(self._handle)
            self._handle = ctypes.c_void_p()
'''

CONTEXT_CLASS = '''class Context:
    """A VmafxContext; close it with close() or a with-block."""

    def __init__(
        self,
        lib: ctypes.CDLL,
        config: ContextConfig | VmafxContextConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> None:
        self._lib = lib
        self._close_attempts = 0
        self._window_refs: list[object] = []
        self._window_state = _CallbackState()
        self._log_thunk, self._log_state = _make_log_thunk(log_callback)
        raw: VmafxContextConfig | None = None
        if isinstance(config, VmafxContextConfig):
            raw = config
        elif config is not None:
            raw = config.to_c()
        elif log_callback is not None:
            raw = VmafxContextConfig()
            raw.struct_size = ctypes.sizeof(raw)
        if raw is not None and log_callback is not None:
            raw.log_callback = self._log_thunk
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        pointer = ctypes.byref(raw) if raw is not None else None
        status = lib.vmafx_context_create(pointer, ctypes.byref(handle), ctypes.byref(error))
        _raise(lib, status, error, "vmafx_context_create", self._log_state)
        self._handle = handle

    def __enter__(self) -> Context:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        if not getattr(self, "_handle", None):
            return
        while self._handle and self._close_attempts < _MAX_CLOSE_ATTEMPTS:
            with contextlib.suppress(Exception):  # __del__ must never raise (ADR-1336)
                self.close()

    def _check_callbacks(self) -> None:
        if self._log_state is not None:
            self._log_state.reraise()
        self._window_state.reraise()

    def close(self) -> None:
        """Destroy the context; on failure it stays open and close() may be retried (ADR-1336)."""
        if not self._handle:
            return
        self._close_attempts += 1
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_destroy(self._handle, ctypes.byref(error))
        try:
            _raise(self._lib, status, error, "vmafx_context_destroy", self._log_state)
        except BaseException:
            raise
        self._handle = ctypes.c_void_p()
        self._window_refs.clear()
        self._check_callbacks()

    def libvmaf_handle(self) -> int:
        """The libvmaf VmafContext * bound to this context, for calls not yet migrated."""
        return int(self._lib.vmafx_context_libvmaf_handle(self._handle) or 0)

    def use_feature(self, extractor: str, options: Options | None = None) -> None:
        """Enable a feature extractor (`vmafx_context_use_feature`)."""
        error = ctypes.c_void_p()
        opt_ptr = options._handle if options is not None else None
        status = self._lib.vmafx_context_use_feature(
            self._handle, extractor.encode(), opt_ptr, ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_context_use_feature", self._log_state)

    def use_model(self, model: Model) -> None:
        """Enable a loaded model (`vmafx_context_use_model`)."""
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_use_model(self._handle, model._handle, ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_context_use_model", self._log_state)

    def use_model_set(self, model_set: ModelSet) -> None:
        """Enable a loaded model set (`vmafx_context_use_model_set`)."""
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_use_model_set(
            self._handle, model_set._handle, ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_context_use_model_set", self._log_state)

    def extractor_count(self) -> int:
        """Number of active feature extractors (`vmafx_context_extractor_count`)."""
        return int(self._lib.vmafx_context_extractor_count(self._handle))

    def feature_resolve(
        self,
        extractor: str,
        options: Options | None = None,
        frame: FrameDesc | VmafxFrameDesc | None = None,
    ) -> FeatureResolution:
        """Preview which backend an extractor would use (`vmafx_feature_resolve`)."""
        raw_frame = frame if isinstance(frame, VmafxFrameDesc) or frame is None else frame.to_c()
        out = VmafxFeatureResolution()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        opt_ptr = options._handle if options is not None else None
        frame_ptr = ctypes.byref(raw_frame) if raw_frame is not None else None
        status = self._lib.vmafx_feature_resolve(
            self._handle,
            extractor.encode(),
            opt_ptr,
            frame_ptr,
            ctypes.byref(out),
            ctypes.byref(error),
        )
        _raise(self._lib, status, error, "vmafx_feature_resolve", self._log_state)
        return FeatureResolution.from_c(out)

    def frame_retention(self) -> int:
        """Max in-flight frames retained by active extractors (`vmafx_context_frame_retention`)."""
        return int(self._lib.vmafx_context_frame_retention(self._handle))

    def submit(self, reference: Frame | None, distorted: Frame | None, index: int) -> None:
        """Submit frame pair `index`, consuming `reference` and `distorted` (`vmafx_submit`)."""
        error = ctypes.c_void_p()
        ref_ptr = reference._handle if reference is not None else None
        dist_ptr = distorted._handle if distorted is not None else None
        if reference is not None and reference is distorted and ref_ptr:
            self._lib.vmafx_frame_ref(ref_ptr)
        if reference is not None:
            reference._handle = ctypes.c_void_p()
        if distorted is not None:
            distorted._handle = ctypes.c_void_p()
        status = self._lib.vmafx_submit(self._handle, ref_ptr, dist_ptr, index, ctypes.byref(error))
        self._check_callbacks()
        _raise(self._lib, status, error, "vmafx_submit", self._log_state)

    def use_device(self, device: Device) -> None:
        """Bind the context to `device` (`vmafx_context_use_device`)."""
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_use_device(
            self._handle, device._handle, ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_context_use_device", self._log_state)

    def admit(self, frame: Frame) -> None:
        """Validate `frame` against the context's device before submit (`vmafx_context_admit`)."""
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_admit(self._handle, frame._handle, ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_context_admit", self._log_state)

    def score_frame(self, model: Model, index: int) -> Score:
        """Read the per-frame score of `model` at `index` (`vmafx_score_frame`)."""
        out = VmafxScore()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_score_frame(
            self._handle, model._handle, index, ctypes.byref(out), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_score_frame", self._log_state)
        return Score.from_c(out)

    def score_frame_model_set(self, model_set: ModelSet, index: int) -> ModelSetScore:
        """Read the per-frame score of `model_set` at `index` (`vmafx_score_frame_model_set`)."""
        out = VmafxModelSetScore()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_score_frame_model_set(
            self._handle, model_set._handle, index, ctypes.byref(out), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_score_frame_model_set", self._log_state)
        return ModelSetScore.from_c(out)

    def score_pooled(self, model: Model, pool: int, first: int, last: int) -> PooledScore:
        """Pool `model` scores over `first..last` (`vmafx_score_pooled`)."""
        out = VmafxPooledScore()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_score_pooled(
            self._handle,
            model._handle,
            pool,
            first,
            last,
            ctypes.byref(out),
            ctypes.byref(error),
        )
        _raise(self._lib, status, error, "vmafx_score_pooled", self._log_state)
        return PooledScore.from_c(out)

    def score_pooled_model_set(
        self, model_set: ModelSet, pool: int, first: int, last: int
    ) -> ModelSetScore:
        """Pool `model_set` scores over `first..last` (`vmafx_score_pooled_model_set`)."""
        out = VmafxModelSetScore()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_score_pooled_model_set(
            self._handle,
            model_set._handle,
            pool,
            first,
            last,
            ctypes.byref(out),
            ctypes.byref(error),
        )
        _raise(self._lib, status, error, "vmafx_score_pooled_model_set", self._log_state)
        return ModelSetScore.from_c(out)

    def provenance_json(self, flags: int = 0) -> str:
        """Render the context's provenance record as JSON (`vmafx_context_provenance_json`)."""
        out = ctypes.c_char_p()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_provenance_json(
            self._handle, flags, ctypes.byref(out), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_context_provenance_json", self._log_state)
        return _text(out.value) or ""

    def window_submit(
        self,
        *,
        target: int,
        pool_mask: int,
        first: int,
        last: int,
        model: Model | None = None,
        model_set: ModelSet | None = None,
        feature: str | None = None,
        on_complete: Callable[[WindowResult], None] | None = None,
    ) -> Window:
        """Submit an asynchronous window (`vmafx_window_submit`)."""
        state = _CallbackState()
        context_state = self._window_state
        feat_b = feature.encode() if feature is not None else None
        if on_complete is not None:

            def _c_window(_win: int | None, result_ptr: object, _user: int | None) -> None:
                try:
                    raw_res = ctypes.cast(result_ptr, ctypes.POINTER(VmafxWindowResult)).contents  # type: ignore[arg-type]  # ctypes pointer
                    on_complete(WindowResult.from_c(raw_res))
                except BaseException as exc:  # FFI boundary must not unwind into C
                    state.record(exc)
                    context_state.record(exc)

            thunk = VmafxWindowCallback(_c_window)
        else:
            thunk = VmafxWindowCallback()
        req = VmafxWindowRequest()
        req.struct_size = ctypes.sizeof(req)
        req.target = target
        req.pool_mask = pool_mask
        req.first = first
        req.last = last
        req.model = model._handle if model is not None else None
        req.model_set = model_set._handle if model_set is not None else None
        req.feature = feat_b
        if on_complete is not None:
            req.on_complete = thunk
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        ref_bundle = (thunk, feat_b, model, model_set, on_complete)
        self._window_refs.append(ref_bundle)
        status = self._lib.vmafx_window_submit(
            self._handle, ctypes.byref(req), ctypes.byref(handle), ctypes.byref(error)
        )
        state.reraise()
        _raise(self._lib, status, error, "vmafx_window_submit", self._log_state)
        return Window(self._lib, handle, ref_bundle, state, self._window_state)

    def max_in_flight(self) -> int:
        """Frames in flight before window backpressure (`vmafx_context_max_in_flight`)."""
        return int(self._lib.vmafx_context_max_in_flight(self._handle))

    def backend(self) -> int:
        """Active `VmafxBackend` of the context (`vmafx_context_backend`)."""
        return int(self._lib.vmafx_context_backend(self._handle))

    def preallocate(self, desc: FrameDesc, count: int) -> None:
        """Pre-allocate a context frame pool (`vmafx_context_preallocate`)."""
        raw = desc.to_c()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_preallocate(
            self._handle, ctypes.byref(raw), count, ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_context_preallocate", self._log_state)

    def acquire_frame(self) -> Frame:
        """Acquire a pre-allocated host frame (`vmafx_context_acquire_frame`)."""
        handle = ctypes.c_void_p()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_acquire_frame(
            self._handle, ctypes.byref(handle), ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_context_acquire_frame", self._log_state)
        return Frame(self._lib, handle)

    def attach_sidedata(self, index: int, data: object) -> None:
        """Attach buffer-protocol codec side-data to frame `index` (`vmafx_context_attach_sidedata`)."""
        mv, addr = _buffer_view(data, writable=False, label="data")
        try:
            error = ctypes.c_void_p()
            status = self._lib.vmafx_context_attach_sidedata(
                self._handle, index, addr, mv.nbytes, ctypes.byref(error)
            )
            _raise(self._lib, status, error, "vmafx_context_attach_sidedata", self._log_state)
        finally:
            mv.release()

    def set_default_color(
        self,
        reference: Color | VmafxColor | None = None,
        distorted: Color | VmafxColor | None = None,
    ) -> None:
        """Set default input color metadata (`vmafx_context_set_default_color`)."""
        raw_ref = (
            reference
            if isinstance(reference, VmafxColor) or reference is None
            else reference.to_c()
        )
        raw_dist = (
            distorted
            if isinstance(distorted, VmafxColor) or distorted is None
            else distorted.to_c()
        )
        ref_ptr = ctypes.byref(raw_ref) if raw_ref is not None else None
        dist_ptr = ctypes.byref(raw_dist) if raw_dist is not None else None
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_set_default_color(
            self._handle, ref_ptr, dist_ptr, ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_context_set_default_color", self._log_state)

    def use_tiny_model(
        self, onnx_path: str, config: DnnConfig | VmafxDnnConfig | None = None
    ) -> None:
        """Register an ONNX tiny-AI model (`vmafx_context_use_tiny_model`)."""
        raw = config if isinstance(config, VmafxDnnConfig) or config is None else config.to_c()
        cfg_ptr = ctypes.byref(raw) if raw is not None else None
        error = ctypes.c_void_p()
        status = self._lib.vmafx_context_use_tiny_model(
            self._handle, onnx_path.encode(), cfg_ptr, ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_context_use_tiny_model", self._log_state)

    def is_codec_aware(self) -> bool:
        """Whether the registered tiny-AI model is codec-aware (`vmafx_context_is_codec_aware`)."""
        return bool(self._lib.vmafx_context_is_codec_aware(self._handle))

    def mcp_server(self, config: McpConfig | VmafxMcpConfig | None = None) -> McpServer:
        """Create an embedded MCP server bound to this context (`vmafx_mcp_server_create`)."""
        return McpServer(self._lib, self, config)
'''

LIBRARY_CLASS = '''class Library:
    """The loaded VMAFx library."""

    def __init__(self, path: str | None = None) -> None:
        self._lib = load(path)

    @property
    def raw(self) -> ctypes.CDLL:
        """The ctypes library, for calls the binding does not wrap."""
        return self._lib

    def context(
        self,
        config: ContextConfig | VmafxContextConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> Context:
        """Create a context (`vmafx_context_create`)."""
        return Context(self._lib, config, log_callback=log_callback)

    def options(self, values: Mapping[str, str] | None = None) -> Options:
        """Create a key-value options list (`vmafx_options_set`)."""
        return Options(self._lib, values)

    def device(self, desc: DeviceDesc | VmafxDeviceDesc | None = None) -> Device:
        """Open a compute device (`vmafx_device_create`)."""
        return Device(self._lib, desc)

    def device_count(self, backend: int) -> int:
        """Count visible devices for `backend` (`vmafx_device_count`)."""
        count = ctypes.c_uint32()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_device_count(backend, ctypes.byref(count), ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_device_count")
        return int(count.value)

    def device_info(self, backend: int, index: int) -> DeviceInfo:
        """Describe device `index` of `backend` (`vmafx_device_info`)."""
        out = VmafxDeviceInfo()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        status = self._lib.vmafx_device_info(backend, index, ctypes.byref(out), ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_device_info")
        return DeviceInfo.from_c(out)

    def frame_create_host(self, desc: FrameDesc, device: Device | None = None) -> Frame:
        """Allocate a host frame (`vmafx_frame_create_host`)."""
        return Frame.create_host(self._lib, desc, device)

    def frame_wrap_host(
        self,
        desc: FrameDesc,
        planes: Sequence[object],
        strides: Sequence[int] | None = None,
        *,
        device: Device | None = None,
        on_release: Callable[[], None] | None = None,
    ) -> Frame:
        """Wrap buffer-protocol planes (`vmafx_frame_wrap_host`)."""
        return Frame.wrap_host(
            self._lib, desc, planes, strides, device=device, on_release=on_release
        )

    def fence_create(self, kind: int, device: Device | None = None) -> Fence:
        """Create a fence (`vmafx_fence_create`)."""
        out = VmafxFence()
        out.struct_size = ctypes.sizeof(out)
        error = ctypes.c_void_p()
        dev_ptr = device._handle if device is not None else None
        status = self._lib.vmafx_fence_create(dev_ptr, kind, ctypes.byref(out), ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_fence_create")
        return Fence.from_c(out)

    def fence_signal(self, fence: Fence | VmafxFence) -> None:
        """Signal a host fence (`vmafx_fence_signal`)."""
        raw = fence if isinstance(fence, VmafxFence) else fence.to_c()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_fence_signal(ctypes.byref(raw), ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_fence_signal")

    def fence_wait(
        self, fence: Fence | VmafxFence, timeout_ns: int = 0xFFFF_FFFF_FFFF_FFFF
    ) -> None:
        """Wait on `fence` up to `timeout_ns` nanoseconds (`vmafx_fence_wait`)."""
        raw = fence if isinstance(fence, VmafxFence) else fence.to_c()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_fence_wait(ctypes.byref(raw), timeout_ns, ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_fence_wait")

    def fence_destroy(self, fence: Fence | VmafxFence) -> None:
        """Release resources owned by `fence` (`vmafx_fence_destroy`)."""
        raw = fence if isinstance(fence, VmafxFence) else fence.to_c()
        error = ctypes.c_void_p()
        status = self._lib.vmafx_fence_destroy(ctypes.byref(raw), ctypes.byref(error))
        _raise(self._lib, status, error, "vmafx_fence_destroy")

    def frame_pool(self, desc: FrameDesc, count: int, device: Device | None = None) -> FramePool:
        """Create a reusable frame pool (`vmafx_frame_pool_create`)."""
        return FramePool(self._lib, desc, count, device)

    def frame_converter(self, desc: ConvertDesc | VmafxConvertDesc) -> FrameConverter:
        """Create a host frame converter (`vmafx_frame_converter_create`)."""
        return FrameConverter(self._lib, desc)

    def model_load(
        self,
        version: str | None = None,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> Model:
        """Load a built-in model (`vmafx_model_load`)."""
        return Model.load(self._lib, version, config, log_callback=log_callback)

    def model_load_file(
        self,
        path: str,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> Model:
        """Load a model from `path` (`vmafx_model_load_file`)."""
        return Model.load_file(self._lib, path, config, log_callback=log_callback)

    def model_load_spec(
        self,
        spec: str,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> Model:
        """Load a model from a CLI spec (`vmafx_model_load_spec`)."""
        return Model.load_spec(self._lib, spec, config, log_callback=log_callback)

    def model_set_load(
        self,
        version: str,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> ModelSet:
        """Load a built-in model collection (`vmafx_model_set_load`)."""
        return ModelSet.load(self._lib, version, config, log_callback=log_callback)

    def model_set_load_file(
        self,
        path: str,
        config: ModelConfig | VmafxModelConfig | None = None,
        *,
        log_callback: Callable[[int, str], None] | None = None,
    ) -> ModelSet:
        """Load a model collection from `path` (`vmafx_model_set_load_file`)."""
        return ModelSet.load_file(self._lib, path, config, log_callback=log_callback)

    def report_open(self, path: str) -> ReportFile:
        """Open a JSON or XML report (`vmafx_report_open`)."""
        return ReportFile(self._lib, path)

    def report_verify(self, recorded: ReportFile, rerun: ReportFile) -> None:
        """Verify two reports (`vmafx_report_verify`)."""
        recorded.verify(rerun)

    def window_clock(self, config: WindowClockConfig | VmafxWindowClockConfig) -> WindowClock:
        """Create a tumbling window clock (`vmafx_window_clock_create`)."""
        return WindowClock(self._lib, config)

    def dnn_available(self) -> bool:
        """Whether tiny-AI ONNX Runtime support is built in (`vmafx_dnn_available`)."""
        return bool(self._lib.vmafx_dnn_available())

    def dnn_verify_signature(self, onnx_path: str, registry_path: str | None = None) -> None:
        """Verify an ONNX model's signature (`vmafx_dnn_verify_signature`)."""
        error = ctypes.c_void_p()
        reg_b = registry_path.encode() if registry_path is not None else None
        status = self._lib.vmafx_dnn_verify_signature(
            onnx_path.encode(), reg_b, ctypes.byref(error)
        )
        _raise(self._lib, status, error, "vmafx_dnn_verify_signature")

    def dnn_session(
        self, onnx_path: str, config: DnnConfig | VmafxDnnConfig | None = None
    ) -> DnnSession:
        """Open an ONNX Runtime session (`vmafx_dnn_session_open`)."""
        return DnnSession(self._lib, onnx_path, config)

    def mcp_available(self) -> bool:
        """Whether the embedded MCP server is built in (`vmafx_mcp_available`)."""
        return bool(self._lib.vmafx_mcp_available())

    def mcp_transport_available(self, transport: int) -> bool:
        """Whether `transport` is supported (`vmafx_mcp_transport_available`)."""
        return bool(self._lib.vmafx_mcp_transport_available(transport))
'''
