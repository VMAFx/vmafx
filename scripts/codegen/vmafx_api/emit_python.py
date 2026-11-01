# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Python binding (bindings/python/vmafx/_api.py): ctypes, standard library only.

Two layers from one definition: the C layer (ctypes structs, enums, function
signatures, the layout the definition implies) and an idiomatic layer
(`Library`, `Context`, `Device`, `Frame`, `FramePool`, `FrameConverter`,
`Model`, `ModelSet`, `Options`, `ReportFile`, `Window`, `WindowClock`,
`DnnSession`, `McpServer`, frozen dataclasses for records, `VmafxError` raised
on every non-OK status). The module checks its struct layouts against the
definition when it is imported, so a binding that disagrees with the header
fails before it touches the library.
"""

from __future__ import annotations

from collections.abc import Callable

from . import typesys
from .layout import by_value_order, layouts
from .model import Api, Callback, DefinitionError, Field, Function, Param, Struct, upper_snake
from .python_runtime import CONTEXT_CLASS, HANDLE_CLASSES, LIBRARY_CLASS, RUNTIME

CTYPES = {
    "u32": "ctypes.c_uint32",
    "u64": "ctypes.c_uint64",
    "i32": "ctypes.c_int32",
    "i64": "ctypes.c_int64",
    "f32": "ctypes.c_float",
    "f64": "ctypes.c_double",
    "uptr": "ctypes.c_size_t",
    "size": "ctypes.c_size_t",
    "ptr": "ctypes.c_void_p",
    "cptr": "ctypes.c_void_p",
    "status": "ctypes.c_int32",
    "cstr": "ctypes.c_char_p",
}
PYTYPES = {
    "u32": "int",
    "u64": "int",
    "i32": "int",
    "i64": "int",
    "f32": "float",
    "f64": "float",
    "uptr": "int",
    "size": "int",
    "status": "int",
    "cstr": "str | None",
}
CONTEXT = "VmafxContext"
POINTERS = ("ptr", "cptr")  # untyped memory: no Python value
LINE_LIMIT = 100  # black line length (pyproject.toml)

NOT_WRAPPED_REASONS: dict[str, str] = {
    "vmafx_frame_import": (
        "imports external device memory handles (DMA-BUF, Vulkan, Win32, Metal, GL, "
        "device pointers); use Library.raw.vmafx_frame_import"
    ),
    "vmafx_context_import_frame": (
        "imports and admits external device memory handles onto a context; "
        "use Library.raw.vmafx_context_import_frame"
    ),
    "vmafx_context_from_libvmaf": (
        "borrows a raw libvmaf VmafContext pointer from the C compatibility layer; "
        "use Library.raw.vmafx_context_from_libvmaf"
    ),
    "vmafx_frame_from_picture": (
        "adopts a raw libvmaf VmafPicture struct pointer from the C compatibility layer; "
        "use Library.raw.vmafx_frame_from_picture"
    ),
    "vmafx_frame_to_picture": (
        "moves a frame into a raw libvmaf VmafPicture struct pointer for the C "
        "compatibility layer; use Library.raw.vmafx_frame_to_picture"
    ),
    "vmafx_model_from_libvmaf": (
        "borrows a raw libvmaf VmafModel pointer from the C compatibility layer; "
        "use Library.raw.vmafx_model_from_libvmaf"
    ),
    "vmafx_model_libvmaf_handle": (
        "returns a raw libvmaf VmafModel pointer for the C compatibility layer; "
        "use Library.raw.vmafx_model_libvmaf_handle"
    ),
    "vmafx_model_set_from_libvmaf": (
        "borrows a raw libvmaf VmafModelCollection pointer from the C compatibility layer; "
        "use Library.raw.vmafx_model_set_from_libvmaf"
    ),
    "vmafx_model_set_libvmaf_handle": (
        "returns a raw libvmaf VmafModelCollection pointer for the C compatibility layer; "
        "use Library.raw.vmafx_model_set_libvmaf_handle"
    ),
}

HANDWRITTEN_WRAPPED: frozenset[str] = frozenset(
    {
        "vmafx_error_status",
        "vmafx_error_message",
        "vmafx_error_subject",
        "vmafx_error_errno",
        "vmafx_error_free",
        "vmafx_error_subject_kind",
        "vmafx_error_function",
        "vmafx_context_create",
        "vmafx_context_destroy",
        "vmafx_context_libvmaf_handle",
        "vmafx_options_set",
        "vmafx_options_free",
        "vmafx_context_use_feature",
        "vmafx_context_use_model",
        "vmafx_context_use_model_set",
        "vmafx_context_extractor_count",
        "vmafx_feature_resolve",
        "vmafx_context_frame_retention",
        "vmafx_submit",
        "vmafx_device_create",
        "vmafx_device_ref",
        "vmafx_device_unref",
        "vmafx_device_backend",
        "vmafx_device_count",
        "vmafx_device_info",
        "vmafx_device_describe",
        "vmafx_device_profile",
        "vmafx_context_use_device",
        "vmafx_frame_create_host",
        "vmafx_frame_wrap_host",
        "vmafx_frame_planes",
        "vmafx_frame_ref",
        "vmafx_frame_unref",
        "vmafx_frame_release_fence",
        "vmafx_frame_signal_on_release",
        "vmafx_fence_create",
        "vmafx_fence_signal",
        "vmafx_fence_wait",
        "vmafx_fence_destroy",
        "vmafx_frame_pool_create",
        "vmafx_frame_pool_acquire",
        "vmafx_frame_pool_destroy",
        "vmafx_context_admit",
        "vmafx_model_load",
        "vmafx_model_load_file",
        "vmafx_model_load_spec",
        "vmafx_model_override_feature",
        "vmafx_model_ref",
        "vmafx_model_unref",
        "vmafx_model_name",
        "vmafx_model_feature_count",
        "vmafx_model_feature_name",
        "vmafx_model_hash",
        "vmafx_model_set_load",
        "vmafx_model_set_load_file",
        "vmafx_model_set_override_feature",
        "vmafx_model_set_ref",
        "vmafx_model_set_unref",
        "vmafx_model_set_lead",
        "vmafx_model_set_size",
        "vmafx_model_set_hash",
        "vmafx_score_frame",
        "vmafx_score_frame_model_set",
        "vmafx_score_pooled",
        "vmafx_score_pooled_model_set",
        "vmafx_context_provenance_json",
        "vmafx_report_open",
        "vmafx_report_close",
        "vmafx_report_field",
        "vmafx_report_verify",
        "vmafx_window_submit",
        "vmafx_window_poll",
        "vmafx_window_wait",
        "vmafx_window_release",
        "vmafx_context_max_in_flight",
        "vmafx_window_clock_create",
        "vmafx_window_clock_frame",
        "vmafx_window_clock_finish",
        "vmafx_window_clock_destroy",
        "vmafx_context_backend",
        "vmafx_context_preallocate",
        "vmafx_context_acquire_frame",
        "vmafx_context_attach_sidedata",
        "vmafx_context_set_default_color",
        "vmafx_frame_converter_create",
        "vmafx_frame_convert",
        "vmafx_frame_converter_destroy",
        "vmafx_dnn_available",
        "vmafx_context_use_tiny_model",
        "vmafx_context_is_codec_aware",
        "vmafx_dnn_session_open",
        "vmafx_dnn_session_run_luma8",
        "vmafx_dnn_session_run_plane16",
        "vmafx_dnn_session_run",
        "vmafx_dnn_session_close",
        "vmafx_dnn_session_runtime_device",
        "vmafx_dnn_verify_signature",
        "vmafx_mcp_available",
        "vmafx_mcp_transport_available",
        "vmafx_mcp_server_create",
        "vmafx_mcp_start_sse",
        "vmafx_mcp_start_uds",
        "vmafx_mcp_start_stdio",
        "vmafx_mcp_stop",
        "vmafx_mcp_server_destroy",
    }
)


def py_seq(items: list[str], indent: str) -> str:
    """A tuple in black's style: `()`, `(x,)`, or exploded with magic trailing commas."""
    if not items:
        return "()"
    if len(items) == 1:
        return f"({items[0]},)"
    inner = "".join(f"{indent}    {item},\n" for item in items)
    return f"(\n{inner}{indent})"


def py_call(head: str, args: list[str], indent: str) -> str:
    """`head(args)` as black writes it at `indent` (right-hand split when too long)."""
    single = f"{indent}{head}({', '.join(args)})"
    if len(single) <= LINE_LIMIT:
        return single
    joined = f"{indent}    {', '.join(args)}"
    if len(joined) <= LINE_LIMIT:
        return f"{indent}{head}(\n{joined}\n{indent})"
    exploded = "".join(f"{indent}    {arg},\n" for arg in args)
    return f"{indent}{head}(\n{exploded}{indent})"


def short(name: str) -> str:
    """VmafxScore -> Score."""
    return name[len("Vmafx") :] if name.startswith("Vmafx") else name


def _ctype_of_param(api: Api, param: Param) -> str:
    if param.mode in ("out_handle", "error"):
        return "ctypes.POINTER(ctypes.c_void_p)"
    kind = typesys.kind(api, param.type)
    if kind == "struct":
        return f"ctypes.POINTER({param.type})"
    if kind in ("handle", "foreign"):
        return "ctypes.c_void_p"
    if kind == "callback":
        return param.type
    base = CTYPES[param.type]
    return f"ctypes.POINTER({base})" if param.mode == "out" else base


def _ctype_of_field(api: Api, fld: Field) -> str:
    kind = typesys.kind(api, fld.type)
    base = {"struct": fld.type, "callback": fld.type, "handle": "ctypes.c_void_p"}.get(kind)
    base = base or CTYPES[fld.type]
    return f"{base} * {fld.count}" if fld.count else base


def _ctype_of_return(returns: str) -> str:
    if returns == "void":
        return "None"
    if returns.startswith(("handle:", "foreign:")):
        return "ctypes.c_void_p"
    return CTYPES[returns]


def _enum_classes(api: Api) -> str:
    out = ["class Status(enum.IntEnum):\n", '    """VmafxStatus codes."""\n\n']
    out += [f"    {s.name.removeprefix('VMAFX_')} = {s.value}\n" for s in api.statuses]
    for item in api.enums:
        prefix = "VMAFX_" + "_".join(item.values[0].name.split("_")[1:-1]) + "_"
        out.append(f"\n\nclass {short(item.name)}(enum.IntEnum):\n")
        out.append(f'    """{item.name}."""\n\n')
        out += [f"    {v.name.removeprefix(prefix)} = {v.value}\n" for v in item.values]
    for flags in api.flags:
        prefix = upper_snake(flags.name).removesuffix("FLAGS").rstrip("_") + "_"
        out.append(f"\n\nclass {short(flags.name)}(enum.IntFlag):\n")
        out.append(f'    """{flags.name} bits."""\n\n')
        out += [f"    {b.name.removeprefix(prefix)} = {1 << b.bit}\n" for b in flags.bits]
    return "".join(out)


def _struct_class(item: Struct) -> str:
    """Declared first, fields set below: a callback or a nested struct may refer to it."""
    return f'class {item.name}(ctypes.Structure):\n    """C struct {item.name}."""\n'


def _callback_type(api: Api, item: Callback) -> str:
    args = [_ctype_of_return(item.returns)] + [_ctype_of_param(api, p) for p in item.params]
    return py_call(f"{item.name} = ctypes.CFUNCTYPE", args, "")


def _struct_fields(api: Api) -> str:
    """`T._fields_ = (...)` for every struct, nested structs first."""
    out = []
    for item in by_value_order(api):
        rows = [f'("{f.name}", {_ctype_of_field(api, f)})' for f in item.fields]
        out.append(f"{item.name}._fields_ = {py_seq(rows, '')}\n")
    return "\n".join(out)


def _layout_table(api: Api) -> str:
    out = ["LAYOUT = {\n"]
    for name, lay in layouts(api).items():
        offsets = py_seq([f'("{f.name}", {f.offset})' for f in lay.fields], "        ")
        out.append(f"    {name}: {py_seq([str(lay.size), offsets], '    ')},\n")
    out.append("}\n")
    return "".join(out)


def _signature_table(api: Api) -> str:
    out = ["SIGNATURES = {\n"]
    for fn in api.functions:
        args = py_seq([_ctype_of_param(api, p) for p in fn.params], "        ")
        out.append(f'    "{fn.name}": {py_seq([_ctype_of_return(fn.returns), args], "    ")},\n')
    out.append("}\n")
    return "".join(out)


def wrapped_functions(api: Api) -> tuple[str, ...]:
    """Names of C functions in `api` wrapped by the Python idiomatic layer, in definition order."""
    auto = {fn.name for fn in context_methods(api)} | {fn.name for fn in library_functions(api)}
    covered = auto | HANDWRITTEN_WRAPPED
    return tuple(fn.name for fn in api.functions if fn.name in covered)


def not_wrapped(api: Api) -> dict[str, str]:
    """C functions in `api` intentionally left on `Library.raw`, in definition order."""
    return {
        fn.name: NOT_WRAPPED_REASONS[fn.name]
        for fn in api.functions
        if fn.name in NOT_WRAPPED_REASONS
    }


def check_coverage(api: Api) -> None:
    """Refuse a definition function that is neither wrapped nor listed in `NOT_WRAPPED`."""
    wrapped = set(wrapped_functions(api))
    skipped = set(not_wrapped(api))
    overlap = sorted(wrapped & skipped)
    if overlap:
        raise DefinitionError(
            f"Python binding: functions in both wrapped and NOT_WRAPPED: {overlap}"
        )
    missing = [fn.name for fn in api.functions if fn.name not in (wrapped | skipped)]
    if missing:
        raise DefinitionError(
            f"Python binding: functions neither wrapped nor in NOT_WRAPPED: {', '.join(missing)}"
        )


def _coverage_tables(api: Api) -> str:
    out = ["NOT_WRAPPED: dict[str, str] = {\n"]
    for name, reason in not_wrapped(api).items():
        out.append(f'    "{name}": "{reason}",\n')
    out.append("}\n\n")
    items = [f'"{name}"' for name in wrapped_functions(api)]
    out.append(f"WRAPPED_FUNCTIONS: tuple[str, ...] = {py_seq(items, '')}\n")
    return "".join(out)


def _record_value(api: Api, fld: Field) -> tuple[str, str] | None:
    """(annotation, conversion from `raw`) of a record field; None keeps it C-only.

    Pointers (`ptr`, handles, callbacks) stay on the ctypes struct: a frozen
    record cannot own what they point at.
    """
    kind = typesys.kind(api, fld.type)
    raw = f"raw.{fld.name}"
    if kind == "struct":
        record = short(fld.type)
        if fld.count:
            return f"tuple[{record}, ...]", f"tuple({record}.from_c(x) for x in {raw})"
        return record, f"{record}.from_c({raw})"
    if kind != "scalar" or fld.type in POINTERS:
        return None
    value = f"_text({raw})" if fld.type == "cstr" else raw
    if fld.count:
        return f"tuple[{PYTYPES[fld.type]}, ...]", f"tuple({raw})"
    return PYTYPES[fld.type], value


def _number(api: Api, fld: Field) -> bool:
    """A single non-pointer number."""
    return (
        typesys.kind(api, fld.type) == "scalar"
        and fld.type not in ("cstr", *POINTERS)
        and not fld.count
    )


def _nested_numbers(api: Api, fld: Field) -> bool:
    """A single struct of numbers (VmafxFrameDesc.color): its record converts back."""
    if typesys.kind(api, fld.type) != "struct" or fld.count:
        return False
    inner = [f for f in api.struct(fld.type).fields if f.name != "struct_size"]
    return all(_number(api, f) for f in inner)


def _plain(api: Api, fields: list[Field]) -> bool:
    """A record converts back to C when every field is a single non-pointer number
    or a single struct of such numbers."""
    return all(_number(api, f) or _nested_numbers(api, f) for f in fields)


def _to_c(api: Api, item: Struct, fields: list[Field]) -> list[str]:
    lines = [f"\n    def to_c(self) -> {item.name}:\n", f"        raw = {item.name}()\n"]
    if item.sized:
        lines.append("        raw.struct_size = ctypes.sizeof(raw)\n")
    for f in fields:
        value = f"self.{f.name}.to_c()" if _nested_numbers(api, f) else f"self.{f.name}"
        lines.append(f"        raw.{f.name} = {value}\n")
    return [*lines, "        return raw\n"]


def _record_class(api: Api, item: Struct) -> str:
    fields = [f for f in item.fields if f.name != "struct_size"]
    values = [(f, _record_value(api, f)) for f in fields]
    kept = [(f, v) for f, v in values if v is not None]
    lines = [f"@dataclass(frozen=True)\nclass {short(item.name)}:\n"]
    lines.append(f'    """{item.doc}"""\n\n')
    lines += [f"    {f.name}: {v[0]}\n" for f, v in kept]
    lines.append(
        f"\n    @classmethod\n    def from_c(cls, raw: {item.name}) -> {short(item.name)}:\n"
    )
    lines.append("        return cls(\n")
    lines += [f"            {f.name}={v[1]},\n" for f, v in kept]
    lines.append("        )\n")
    # Pointer fields (`ptr`, handles, callbacks) have no record value and stay
    # NULL in the C struct, so a record whose other fields are plain numbers
    # still converts back (VmafxContextConfig and its log callback).
    plain = [f for f, _ in kept]
    if _plain(api, plain):
        lines += _to_c(api, item, plain)
    return "".join(lines)


def _method_name(fn: Function) -> str:
    """The function's `python` name, else its C name without `vmafx_context_` / `vmafx_`."""
    return fn.python or fn.name.removeprefix("vmafx_context_").removeprefix("vmafx_")


def _library_method_name(fn: Function) -> str:
    return fn.python or fn.name.removeprefix("vmafx_")


def _refuse_collisions(cls: str, fns: list[Function], name_of: Callable[[Function], str]) -> None:
    """Two functions on one class with one Python name would leave the first
    unreachable (Python keeps the last definition): refuse the definition."""
    seen: dict[str, str] = {}
    for fn in fns:
        name = name_of(fn)
        if name in seen:
            raise DefinitionError(
                f"Python binding: {cls}.{name} would bind both {seen[name]} and {fn.name}; "
                "give one of them a `python` name"
            )
        seen[name] = fn.name


def _call_args(api: Api, fn: Function, first: str) -> tuple[list[str], list[str], str]:
    """(setup lines, call arguments, result expression) for one method."""
    setup: list[str] = []
    args: list[str] = []
    result = "None"
    for position, param in enumerate(fn.params):
        if position == 0 and first:
            args.append(first)
        elif param.mode == "error":
            setup.append("error = ctypes.c_void_p()")
            args.append("ctypes.byref(error)")
        elif param.mode == "out" and api.is_struct(param.type):
            setup.append(f"out = {param.type}()")
            setup.append("out.struct_size = ctypes.sizeof(out)")
            args.append("ctypes.byref(out)")
            result = f"{short(param.type)}.from_c(out)"
        elif param.type == "cstr":
            expr = (
                f"{param.name}.encode() if {param.name} is not None else None"
                if param.nullable
                else f"{param.name}.encode()"
            )
            args.append(expr)
        else:
            args.append(param.name)
    return setup, args, result


def _python_params(api: Api, fn: Function, skip_first: bool) -> str:
    names = []
    for position, param in enumerate(fn.params):
        if (position == 0 and skip_first) or param.mode in ("error", "out", "out_handle"):
            continue
        if param.type == "cstr":
            hint = "str | None" if param.nullable else "str"
        else:
            hint = PYTYPES.get(param.type, "int")
        names.append(f"{param.name}: {hint}")
    return "".join(", " + n for n in names)


def _method(api: Api, fn: Function) -> str:
    setup, args, result = _call_args(api, fn, "self._handle")
    params = _python_params(api, fn, skip_first=True)
    ret = result.split(".", 1)[0] if result != "None" else "None"
    body = "".join(f"        {line}\n" for line in setup)
    body += py_call(f"status = self._lib.{fn.name}", args, "        ") + "\n"
    body += "        self._check_callbacks()\n"
    body += f'        _raise(self._lib, status, error, "{fn.name}", self._log_state)\n'
    if result != "None":  # ruff (RET501) refuses a bare `return None`
        body += f"        return {result}\n"
    return f'    def {_method_name(fn)}(self{params}) -> {ret}:\n        """{fn.doc}"""\n' + body


def _bindable(api: Api, param: Param) -> bool:
    """Parameter shapes the auto-emitted Context methods wrap."""
    kind = typesys.kind(api, param.type)
    if param.mode == "error" or (param.mode == "out" and kind == "struct"):
        return True
    return param.mode == "in" and kind == "scalar" and param.type not in POINTERS


def context_methods(api: Api) -> list[Function]:
    """Functions that take a context first and are neither its constructor nor release."""
    release = {h.release for h in api.handles}
    return [
        fn
        for fn in api.functions
        if fn.params
        and fn.params[0].type == CONTEXT
        and fn.returns == "status"
        and fn.name not in release
        and all(_bindable(api, p) for p in fn.params[1:])
    ]


def _module_header(api: Api) -> str:
    major, minor, patch = api.abi_version
    return (
        "# Copyright 2026 Lusoris\n"
        "# " + "SPDX-" + "License-Identifier: EUPL-1.2\n"  # split: not this file's header
        "#\n"
        "# GENERATED by scripts/codegen/vmafx-api.py from core/api/vmafx.toml (ADR-1852).\n"
        "# Do not edit: change the definition and run\n"
        "# `python3 scripts/codegen/vmafx-api.py --write`.\n"
        '"""ctypes binding of the VMAFx C API (generated)."""\n\n'
        "from __future__ import annotations\n\n"
        "import contextlib\nimport ctypes\nimport enum\nimport os\n"
        "from collections.abc import Callable, Mapping, Sequence\n"
        "from dataclasses import dataclass\n\n"
        f"ABI_VERSION = ({major}, {minor}, {patch})\n"
        f'__version__ = "{major}.{minor}.{patch}"\n'
    )


def library_functions(api: Api) -> list[Function]:
    """Functions on no handle: methods of `Library`."""
    out = []
    for fn in api.functions:
        kinds = {typesys.kind(api, p.type) for p in fn.params}
        if kinds - {"scalar"} or any(p.type in POINTERS for p in fn.params):
            continue
        if fn.returns not in ("cstr", "void"):
            continue
        out.append(fn)
    return out


def _library_arg(param: Param) -> str:
    if param.mode == "out":
        return f"ctypes.byref({param.name})"
    if param.type == "cstr":
        return (
            f"{param.name}.encode() if {param.name} is not None else None"
            if param.nullable
            else f"{param.name}.encode()"
        )
    return param.name


def _library_param(param: Param) -> str:
    hint = PYTYPES.get(param.type, "int")
    default = " = None" if param.nullable and param.type == "cstr" else ""
    return f", {param.name}: {hint}{default}"


def _library_method(api: Api, fn: Function) -> str:
    outs = [p for p in fn.params if p.mode == "out"]
    ins = [p for p in fn.params if p.mode != "out"]
    params = "".join(_library_param(p) for p in ins)
    body = "".join(f"        {p.name} = {CTYPES[p.type]}()\n" for p in outs)
    args = ", ".join(_library_arg(p) for p in fn.params)
    call = f"self._lib.{fn.name}({args})"
    if fn.returns == "cstr":
        ret, body = "str | None", body + py_call("return _text", [call], "        ") + "\n"
    else:
        values = ", ".join(f"int({p.name}.value)" for p in outs) + ("," if len(outs) == 1 else "")
        ret, body = "tuple[int, ...]", body + f"        {call}\n        return ({values})\n"
    return (
        f'    def {_library_method_name(fn)}(self{params}) -> {ret}:\n        """{fn.doc}"""\n'
        + body
    )


def pyproject_text(api: Api) -> str:
    """Generated `bindings/python/pyproject.toml`, single-sourced from `core/api/vmafx.toml`."""
    return (
        "# Copyright 2026 Lusoris\n"
        "# " + "SPDX-" + "License-Identifier: EUPL-1.2\n"
        "#\n"
        "# GENERATED by scripts/codegen/vmafx-api.py from core/api/vmafx.toml (ADR-1852).\n"
        "# Do not edit: change the definition and run\n"
        "# `python3 scripts/codegen/vmafx-api.py --write`.\n\n"
        "[build-system]\n"
        'requires = ["hatchling>=1.32.0"]\n'
        'build-backend = "hatchling.build"\n\n'
        "[project]\n"
        f'name = "{api.name}"\n'
        'dynamic = ["version"]\n'
        'description = "ctypes binding for the VMAFx C API (standard library only)."\n'
        'requires-python = ">=3.12"\n'
        'license = "EUPL-1.2"\n'
        'license-files = ["LICENSES/*"]\n'
        'authors = [{ name = "Lusoris", email = "lusoris@pm.me" }]\n'
        "dependencies = []\n\n"
        "[project.urls]\n"
        'Homepage = "https://github.com/VMAFx/vmafx"\n'
        'Repository = "https://github.com/VMAFx/vmafx"\n'
        'Documentation = "https://vmafx.dev/api/vmafx/bindings/"\n'
        'Issues = "https://github.com/VMAFx/vmafx/issues"\n\n'
        "[tool.hatch.version]\n"
        'path = "vmafx/_api.py"\n\n'
        "[tool.hatch.build.targets.wheel]\n"
        'packages = ["vmafx"]\n\n'
        "[tool.setuptools.packages.find]\n"
        'include = ["vmafx*"]\n\n'
        "[tool.setuptools.package-data]\n"
        'vmafx = ["py.typed"]\n\n'
        "[tool.setuptools.dynamic]\n"
        'version = { attr = "vmafx._api.__version__" }\n'
    )


def module_text(api: Api) -> str:
    parts = [_module_header(api), _enum_classes(api)]
    parts += [_struct_class(item) for item in api.structs]
    parts += [_callback_type(api, item) for item in api.callbacks]
    if api.structs:
        parts.append(_struct_fields(api))
    parts += [_layout_table(api), _signature_table(api), _coverage_tables(api), RUNTIME]
    parts += [_record_class(api, item) for item in api.structs]
    parts.append(HANDLE_CLASSES)
    _refuse_collisions("Context", context_methods(api), _method_name)
    _refuse_collisions("Library", library_functions(api), _library_method_name)
    methods = "\n".join(_method(api, fn) for fn in context_methods(api))
    parts.append(CONTEXT_CLASS + ("\n" + methods if methods else ""))
    lib_methods = "\n".join(_library_method(api, fn) for fn in library_functions(api))
    parts.append(LIBRARY_CLASS + ("\n" + lib_methods if lib_methods else ""))
    parts.append("_check_layout()\n")
    return "\n\n".join(p.rstrip("\n") + "\n" for p in parts)
