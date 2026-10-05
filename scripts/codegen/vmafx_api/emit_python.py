# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Python binding (bindings/python/vmafx/_api.py): ctypes, standard library only.

Two layers from one definition: the C layer (ctypes structs, enums, function
signatures, the layout the definition implies) and an idiomatic layer
(`Library`, `Context`, frozen dataclasses for records, `VmafxError` raised
on every non-OK status). The module checks its struct layouts against the
definition when it is imported, so a binding that disagrees with the header
fails before it touches the library.
"""

from __future__ import annotations

from .layout import struct_layout
from .model import Api, Function, Param, Struct
from .python_runtime import CONTEXT_CLASS, LIBRARY_CLASS, RUNTIME

CTYPES = {
    "u32": "ctypes.c_uint32",
    "u64": "ctypes.c_uint64",
    "i32": "ctypes.c_int32",
    "f64": "ctypes.c_double",
    "status": "ctypes.c_int32",
    "cstr": "ctypes.c_char_p",
}
PYTYPES = {"u32": "int", "u64": "int", "i32": "int", "f64": "float", "cstr": "str | None"}
CONTEXT = "VmafxContext"
LINE_LIMIT = 100  # black line length (pyproject.toml)


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
    if api.is_struct(param.type):
        return f"ctypes.POINTER({param.type})"
    if api.is_handle(param.type) or param.type.startswith("foreign:"):
        return "ctypes.c_void_p"
    base = CTYPES[param.type]
    return f"ctypes.POINTER({base})" if param.mode == "out" else base


def _ctype_of_return(fn: Function) -> str:
    if fn.returns == "void":
        return "None"
    if fn.returns.startswith(("handle:", "foreign:")):
        return "ctypes.c_void_p"
    return CTYPES[fn.returns]


def _enum_classes(api: Api) -> str:
    out = ["class Status(enum.IntEnum):\n", '    """VmafxStatus codes."""\n\n']
    out += [f"    {s.name.removeprefix('VMAFX_')} = {s.value}\n" for s in api.statuses]
    for item in api.enums:
        prefix = "VMAFX_" + "_".join(item.values[0].name.split("_")[1:-1]) + "_"
        out.append(f"\n\nclass {short(item.name)}(enum.IntEnum):\n")
        out.append(f'    """{item.name}."""\n\n')
        out += [f"    {v.name.removeprefix(prefix)} = {v.value}\n" for v in item.values]
    return "".join(out)


def _struct_class(item: Struct) -> str:
    fields = "".join(f'        ("{f.name}", {CTYPES[f.type]}),\n' for f in item.fields)
    return f"class {item.name}(ctypes.Structure):\n    _fields_ = (\n{fields}    )\n"


def _layout_table(api: Api) -> str:
    out = ["LAYOUT = {\n"]
    for item in api.structs:
        lay = struct_layout(item)
        offsets = py_seq([f'("{f.name}", {f.offset})' for f in lay.fields], "        ")
        out.append(f"    {item.name}: {py_seq([str(lay.size), offsets], '    ')},\n")
    out.append("}\n")
    return "".join(out)


def _signature_table(api: Api) -> str:
    out = ["SIGNATURES = {\n"]
    for fn in api.functions:
        args = py_seq([_ctype_of_param(api, p) for p in fn.params], "        ")
        out.append(f'    "{fn.name}": {py_seq([_ctype_of_return(fn), args], "    ")},\n')
    out.append("}\n")
    return "".join(out)


def _record_class(item: Struct) -> str:
    fields = [f for f in item.fields if f.name != "struct_size"]
    lines = [f"@dataclass(frozen=True)\nclass {short(item.name)}:\n"]
    lines.append(f'    """{item.doc}"""\n\n')
    lines += [f"    {f.name}: {PYTYPES[f.type]}\n" for f in fields]
    lines.append(
        f"\n    @classmethod\n    def from_c(cls, raw: {item.name}) -> {short(item.name)}:\n"
    )
    lines.append("        return cls(\n")
    for field in fields:
        value = f"raw.{field.name}"
        if field.type == "cstr":
            value = f"_text(raw.{field.name})"
        lines.append(f"            {field.name}={value},\n")
    lines.append("        )\n")
    if all(f.type != "cstr" for f in fields):
        lines.append(f"\n    def to_c(self) -> {item.name}:\n")
        lines.append(f"        raw = {item.name}()\n        raw.struct_size = ctypes.sizeof(raw)\n")
        lines += [f"        raw.{f.name} = self.{f.name}\n" for f in fields]
        lines.append("        return raw\n")
    return "".join(lines)


def _method_name(fn: Function) -> str:
    return fn.name.removeprefix("vmafx_context_").removeprefix("vmafx_")


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
            args.append(f"{param.name}.encode()")
        else:
            args.append(param.name)
    return setup, args, result


def _python_params(api: Api, fn: Function, skip_first: bool) -> str:
    names = []
    for position, param in enumerate(fn.params):
        if (position == 0 and skip_first) or param.mode in ("error", "out", "out_handle"):
            continue
        hint = "str" if param.type == "cstr" else PYTYPES.get(param.type, "int")
        names.append(f"{param.name}: {hint}")
    return "".join(", " + n for n in names)


def _method(api: Api, fn: Function) -> str:
    setup, args, result = _call_args(api, fn, "self._handle")
    params = _python_params(api, fn, skip_first=True)
    ret = result.split(".", 1)[0] if result != "None" else "None"
    body = "".join(f"        {line}\n" for line in setup)
    body += py_call(f"status = self._lib.{fn.name}", args, "        ") + "\n"
    body += f'        _raise(self._lib, status, error, "{fn.name}")\n'
    body += f"        return {result}\n"
    return f'    def {_method_name(fn)}(self{params}) -> {ret}:\n        """{fn.doc}"""\n' + body


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
        "import ctypes\nimport enum\nimport os\nfrom dataclasses import dataclass\n\n"
        f"ABI_VERSION = ({major}, {minor}, {patch})\n"
    )


def library_functions(api: Api) -> list[Function]:
    """Functions on no handle: methods of `Library`."""
    out = []
    for fn in api.functions:
        typed = [p.type for p in fn.params]
        if any(api.is_handle(t) or t.startswith("foreign:") for t in typed):
            continue
        if fn.returns not in ("cstr", "void"):
            continue
        out.append(fn)
    return out


def _library_method(api: Api, fn: Function) -> str:
    outs = [p for p in fn.params if p.mode == "out"]
    ins = [p for p in fn.params if p.mode != "out"]
    params = "".join(f", {p.name}: {PYTYPES.get(p.type, 'int')}" for p in ins)
    body = "".join(f"        {p.name} = {CTYPES[p.type]}()\n" for p in outs)
    args = ", ".join(f"ctypes.byref({p.name})" if p.mode == "out" else p.name for p in fn.params)
    call = f"self._lib.{fn.name}({args})"
    if fn.returns == "cstr":
        ret, body = "str | None", body + f"        return _text({call})\n"
    else:
        values = ", ".join(f"{p.name}.value" for p in outs) + ("," if len(outs) == 1 else "")
        ret, body = "tuple[int, ...]", body + f"        {call}\n        return ({values})\n"
    name = fn.name.removeprefix("vmafx_")
    return f'    def {name}(self{params}) -> {ret}:\n        """{fn.doc}"""\n' + body


def module_text(api: Api) -> str:
    parts = [_module_header(api), _enum_classes(api)]
    parts += [_struct_class(item) for item in api.structs]
    parts += [_layout_table(api), _signature_table(api), RUNTIME]
    parts += [_record_class(item) for item in api.structs]
    methods = "\n".join(_method(api, fn) for fn in context_methods(api))
    parts.append(CONTEXT_CLASS + "\n" + methods)
    lib_methods = "\n".join(_library_method(api, fn) for fn in library_functions(api))
    parts.append(LIBRARY_CLASS + "\n" + lib_methods)
    parts.append("_check_layout()\n")
    return "\n\n".join(p.rstrip("\n") + "\n" for p in parts)
