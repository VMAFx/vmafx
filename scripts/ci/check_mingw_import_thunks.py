#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check MinGW import library thunks (ADR-3397) and static runtime export leakage.

When a MinGW GCC DLL is linked with LTO, GNU ``ld --out-implib`` emits ``I __imp_<fn>``
pointer entries and ``I __nm_<fn>`` data aliases, omitting the ``T <fn>`` function
thunks.  An executable linked against that import library auto-imports every missing
function call as data, creating a 32-bit PEI runtime pseudo-relocation per call site
that aborts before ``main`` under 64-bit high-entropy ASLR (VMAFx/vmafx#3257).

For every ``*.dll.a`` under ``<build>/src``, this script pairs the archive with its DLL
(``lib<name>.dll`` or ``lib<name>-*.dll`` in ``--dll-dir``, defaulting to ``<build>/src``),
extracts the DLL's function exports (PE export directory entries whose RVA lies in a
``CODE`` section, excluding data exports and untyped assembly labels), and checks that
every function export has a ``T <name>`` thunk in ``nm`` output on the ``.dll.a``.

Additionally, when linking with ``-static``, MinGW GNU ld auto-export re-exports globals
pulled from toolchain static runtime archives (e.g. ``libgcc_eh.a``) such as
``_Unwind_Resume``.  Executables that link the runtime statically and link against the
import library will fail with duplicate symbol definitions.  When ``--runtime-archive``
is specified, this script checks that paired DLLs export no symbols defined by those
runtime archives.
"""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import struct
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import (
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
    executable_argument,
)
from scripts.lib.safe_subprocess import (
    run as run_command,
)

NM_TIMEOUT_SECONDS = 60.0
NM_MAX_OUTPUT_BYTES = 64 * 1_048_576
MAX_PE_SECTIONS = 96
MAX_PE_EXPORTS = 65_536
MAX_COFF_SYMBOLS = 262_144
MAX_SYMBOL_NAME_BYTES = 4096
MAX_MISSING_PREVIEW = 10
MIN_DOS_HEADER_BYTES = 0x40
MIN_PE_HEADER_BYTES = 24
COFF_STRTAB_HEADER_BYTES = 4
MIN_NM_TOKENS = 2
NM_DEFINED_SYMBOL_TOKENS = 3
DEFINED_GLOBAL_TYPES = frozenset({"T", "D", "B", "R", "S", "C"})
PE32_MAGIC = 0x10B
PE32_PLUS_MAGIC = 0x20B
IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SYM_CLASS_EXTERNAL = 2
IMAGE_SYM_TYPE_FUNCTION = 0x20

ToolSpec = str | Sequence[str]
Runner = Callable[[ToolSpec, Path], tuple[int, str, str]]
DllExportsReader = Callable[[Path], frozenset[str]]


@dataclass(frozen=True)
class _PeSection:
    name: str
    vaddr: int
    vspan: int
    raw_ptr: int
    raw_size: int
    characteristics: int

    def contains_rva(self, rva: int) -> bool:
        return self.vaddr <= rva < self.vaddr + self.vspan

    def is_executable(self) -> bool:
        return bool(self.characteristics & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE))

    def rva_to_offset(self, rva: int) -> int:
        delta = rva - self.vaddr
        if delta < 0 or delta >= self.raw_size:
            raise ValueError(f"RVA 0x{rva:x} outside raw data of section {self.name!r}")
        return self.raw_ptr + delta


def _rva_to_file_offset(sections: Sequence[_PeSection], rva: int) -> int:
    for section in sections:
        if section.contains_rva(rva):
            return section.rva_to_offset(rva)
    raise ValueError(f"RVA 0x{rva:x} does not map to any PE section")


def _find_section_for_rva(sections: Sequence[_PeSection], rva: int) -> _PeSection | None:
    for section in sections:
        if section.contains_rva(rva):
            return section
    return None


def _read_cstring(data: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(data):
        raise ValueError(f"C-string offset {offset} out of bounds")
    limit = min(len(data), offset + MAX_SYMBOL_NAME_BYTES)
    end = data.find(b"\x00", offset, limit)
    if end < 0:
        raise ValueError(f"unterminated C-string at offset {offset}")
    return data[offset:end].decode("ascii", "replace")


def _parse_pe_sections(data: bytes, sec_off: int, num_sections: int) -> list[_PeSection]:
    if num_sections <= 0 or num_sections > MAX_PE_SECTIONS:
        raise ValueError(f"invalid PE section count: {num_sections}")
    sections: list[_PeSection] = []
    for idx in range(num_sections):
        off = sec_off + idx * 40
        raw_name, vsize, vaddr, rsize, rptr, _, _, _, _, chars = struct.unpack_from(
            "<8sIIIIIIHHI", data, off
        )
        name = raw_name.rstrip(b"\x00").decode("ascii", "replace")
        sections.append(_PeSection(name, vaddr, max(vsize, rsize), rptr, rsize, chars))
    return sections


def _parse_pe_headers(data: bytes) -> tuple[list[_PeSection], int, int, int, int]:
    if len(data) < MIN_DOS_HEADER_BYTES or data[:2] != b"MZ":
        raise ValueError("not a valid MZ/PE image")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_off + MIN_PE_HEADER_BYTES > len(data) or data[pe_off : pe_off + 4] != b"PE\x00\x00":
        raise ValueError("missing PE\\0\\0 signature")
    _, num_sections, _, sym_off, num_syms, opt_size, _ = struct.unpack_from(
        "<HHIIIHH", data, pe_off + 4
    )
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic not in (PE32_MAGIC, PE32_PLUS_MAGIC):
        raise ValueError(f"unsupported PE optional header magic: 0x{magic:x}")
    dd_off = opt_off + (112 if magic == PE32_PLUS_MAGIC else 96)
    export_rva, export_size = struct.unpack_from("<II", data, dd_off)
    sections = _parse_pe_sections(data, opt_off + opt_size, num_sections)
    return sections, export_rva, export_size, sym_off, num_syms


def _decode_coff_symbol_name(
    name_bytes: bytes, data: bytes, strtab_off: int, strtab_end: int
) -> str:
    if name_bytes[:4] == b"\x00\x00\x00\x00":
        str_off = strtab_off + struct.unpack_from("<I", name_bytes, 4)[0]
        if str_off >= strtab_end:
            raise ValueError("COFF string table offset out of bounds")
        return _read_cstring(data, str_off)
    return name_bytes.rstrip(b"\x00").decode("ascii", "replace")


def _read_coff_symbol_types(data: bytes, sym_off: int, num_syms: int) -> dict[str, int]:
    if sym_off == 0 or num_syms == 0:
        return {}
    if num_syms < 0 or num_syms > MAX_COFF_SYMBOLS:
        raise ValueError(f"invalid COFF symbol count: {num_syms}")
    strtab_off = sym_off + num_syms * 18
    if strtab_off + COFF_STRTAB_HEADER_BYTES > len(data):
        return {}
    strtab_size = struct.unpack_from("<I", data, strtab_off)[0]
    strtab_end = min(len(data), strtab_off + strtab_size)
    types: dict[str, int] = {}
    idx = 0
    for _ in range(num_syms):
        if idx >= num_syms:
            break
        rec_off = sym_off + idx * 18
        name_bytes = data[rec_off : rec_off + 8]
        _, sec_num, sym_type, storage_class, num_aux = struct.unpack_from(
            "<IhHBB", data, rec_off + 8
        )
        if storage_class == IMAGE_SYM_CLASS_EXTERNAL and sec_num > 0:
            name = _decode_coff_symbol_name(name_bytes, data, strtab_off, strtab_end)
            types[name] = sym_type
        idx += 1 + num_aux
    return types


def _iter_pe_exports(
    data: bytes,
    sections: Sequence[_PeSection],
    export_rva: int,
    export_size: int,
    coff_types: dict[str, int],
) -> list[tuple[str, bool]]:
    exp_off = _rva_to_file_offset(sections, export_rva)
    _, _, _, _, _, _, num_funcs, num_names, addr_rva, name_rva, ord_rva = struct.unpack_from(
        "<IIHHIIIIIII", data, exp_off
    )
    if num_names > MAX_PE_EXPORTS or num_funcs > MAX_PE_EXPORTS:
        raise ValueError(f"export count exceeds {MAX_PE_EXPORTS}")
    addr_off = _rva_to_file_offset(sections, addr_rva)
    name_off = _rva_to_file_offset(sections, name_rva)
    ord_off = _rva_to_file_offset(sections, ord_rva)
    exports: list[tuple[str, bool]] = []
    for idx in range(num_names):
        sym_name_rva = struct.unpack_from("<I", data, name_off + idx * 4)[0]
        sym = _read_cstring(data, _rva_to_file_offset(sections, sym_name_rva))
        ord_idx = struct.unpack_from("<H", data, ord_off + idx * 2)[0]
        if ord_idx >= num_funcs:
            raise ValueError(f"ordinal index {ord_idx} out of range ({num_funcs})")
        fn_rva = struct.unpack_from("<I", data, addr_off + ord_idx * 4)[0]
        if export_rva <= fn_rva < export_rva + export_size:
            exports.append((sym, False))
            continue
        sec = _find_section_for_rva(sections, fn_rva)
        is_code = (
            sec is not None
            and sec.is_executable()
            and coff_types.get(sym, IMAGE_SYM_TYPE_FUNCTION) == IMAGE_SYM_TYPE_FUNCTION
        )
        exports.append((sym, is_code))
    return exports


def _read_dll_exports(dll_path: Path) -> list[tuple[str, bool]]:
    try:
        data = dll_path.read_bytes()
        sections, export_rva, export_size, sym_off, num_syms = _parse_pe_headers(data)
        if export_rva == 0 or export_size == 0:
            return []
        coff_types = _read_coff_symbol_types(data, sym_off, num_syms)
        return _iter_pe_exports(data, sections, export_rva, export_size, coff_types)
    except struct.error as exc:
        raise ValueError(f"truncated PE structure in {dll_path}") from exc


def read_dll_function_exports(dll_path: Path) -> frozenset[str]:
    """Return function export names whose RVA lies in a ``CODE`` section of ``dll_path``."""
    return frozenset(sym for sym, is_code in _read_dll_exports(dll_path) if is_code)


def read_dll_all_exports(dll_path: Path) -> frozenset[str]:
    """Return all export names (both function and data) of ``dll_path``."""
    return frozenset(sym for sym, _ in _read_dll_exports(dll_path))


def find_paired_dll(archive: Path, dll_dir: Path) -> Path | None:
    """Find ``lib<name>.dll`` or ``lib<name>-*.dll`` paired with ``lib<name>.dll.a``."""
    if not archive.name.endswith(".dll.a"):
        return None
    stem = archive.name[: -len(".dll.a")]
    exact = dll_dir / f"{stem}.dll"
    if exact.is_file():
        return exact
    matches = sorted(p for p in dll_dir.glob(f"{stem}-*.dll") if p.is_file())
    return matches[0] if len(matches) == 1 else None


def _resolve_single_executable(candidate: str) -> str:
    executable = executable_argument(candidate, Path.cwd())
    found = shutil.which(executable)
    if found is None and Path(executable).is_file():
        found = executable
    if found is None:
        raise ValueError(f"nm binary not found: {candidate!r}")
    return str(Path(found).resolve(strict=True))


def strip_surrounding_quotes(token: str) -> str:
    """Strip one matching pair of surrounding quotes from a token of length 2 or more."""
    if len(token) > 1 and token[0] in ('"', "'") and token[0] == token[-1]:
        return token[1:-1]
    return token


def resolve_nm_command(explicit: ToolSpec | None) -> tuple[str, ...]:
    """Resolve ``--nm``, ``$NM``, or ``nm`` into an executable command prefix."""
    if explicit is not None and not isinstance(explicit, str):
        tokens = [str(item) for item in explicit]
    else:
        raw = explicit or os.environ.get("NM") or "nm"
        raw_path = Path(raw).expanduser()
        if raw_path.is_file():
            tokens = [str(raw_path)]
        elif os.name == "nt":
            tokens = [strip_surrounding_quotes(tok) for tok in shlex.split(raw, posix=False)]
        else:
            tokens = shlex.split(raw, posix=True)
    if not tokens:
        raise ValueError("nm command must not be empty")
    first_path = Path(tokens[0]).expanduser()
    if first_path.suffix.lower() == ".py" and first_path.is_file():
        python_exe = str(Path(sys.executable).resolve(strict=True))
        script_path = str(first_path.resolve(strict=True))
        return (python_exe, script_path, *tokens[1:])
    resolved_exe = _resolve_single_executable(tokens[0])
    return (resolved_exe, *tokens[1:])


def _run_nm(nm_tool: ToolSpec, archive: Path, flags: Sequence[str] = ()) -> tuple[int, str, str]:
    try:
        cmd_prefix = resolve_nm_command(nm_tool)
        proc = run_command(
            [*cmd_prefix, *flags, str(archive)],
            allowed_executables=(cmd_prefix[0],),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=NM_TIMEOUT_SECONDS,
            max_output_bytes=NM_MAX_OUTPUT_BYTES,
        )
    except (
        OSError,
        ValueError,
        CommandValidationError,
        CommandTimedOut,
        CommandOutputLimitExceeded,
    ) as exc:
        return 1, "", str(exc)
    return proc.returncode, proc.stdout, proc.stderr


def default_run_nm(nm_tool: ToolSpec, archive: Path) -> tuple[int, str, str]:
    """Run ``nm`` on ``archive`` with a timeout and bounded output."""
    return _run_nm(nm_tool, archive)


def default_run_nm_defined_globals(nm_tool: ToolSpec, archive: Path) -> tuple[int, str, str]:
    """Run ``nm --defined-only -g`` on ``archive`` with a timeout and bounded output."""
    return _run_nm(nm_tool, archive, ("--defined-only", "-g"))


def parse_nm_thunks(nm_output: str) -> frozenset[str]:
    """Return the set of ``T <name>`` function thunk symbols from ``nm`` text output."""
    thunks: set[str] = set()
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) >= MIN_NM_TOKENS and parts[-2] == "T":
            thunks.add(parts[-1])
    return frozenset(thunks)


def parse_nm_defined_globals(nm_output: str) -> frozenset[str]:
    """Return defined global symbol names from ``nm`` text output."""
    symbols: set[str] = set()
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) == NM_DEFINED_SYMBOL_TOKENS and parts[1] in DEFINED_GLOBAL_TYPES:
            symbols.add(parts[2])
    return frozenset(symbols)


def _check_runtime_overlap(
    dll_path: Path,
    runtime_data: Sequence[tuple[Path, frozenset[str]]],
    all_dll_reader: DllExportsReader,
) -> bool:
    try:
        all_exports = all_dll_reader(dll_path)
    except (OSError, ValueError) as exc:
        print(
            f"check_mingw_import_thunks: FAIL {dll_path}: cannot read DLL exports: {exc}",
            file=sys.stderr,
        )
        return False

    all_passed = True
    for rt_archive, rt_symbols in runtime_data:
        overlap = all_exports & rt_symbols
        if overlap:
            sorted_overlap = sorted(overlap)
            preview = ", ".join(sorted_overlap[:MAX_MISSING_PREVIEW])
            extra = (
                f" (+{len(sorted_overlap) - MAX_MISSING_PREVIEW} more)"
                if len(sorted_overlap) > MAX_MISSING_PREVIEW
                else ""
            )
            print(
                f"check_mingw_import_thunks: FAIL {dll_path}: "
                f"exports {len(overlap)} symbol(s) defined by toolchain runtime archive {rt_archive.name}: "
                f"[{preview}{extra}] "
                "(consumers that link the runtime statically get duplicate definitions, "
                "e.g. multiple definition of _Unwind_Resume; "
                "exclude the archive from auto-export with -Wl,--exclude-libs)",
                file=sys.stderr,
            )
            all_passed = False
        else:
            print(
                f"check_mingw_import_thunks: OK {dll_path} ({rt_archive.name}): "
                "runtime_overlap=0"
            )
    return all_passed


def _load_runtime_archives(
    runtime_archives: Sequence[Path],
    tool: ToolSpec,
    runner: Runner,
) -> tuple[list[tuple[Path, frozenset[str]]], bool]:
    results: list[tuple[Path, frozenset[str]]] = []
    ok = True
    for rt_path in runtime_archives:
        if not rt_path.is_file():
            print(
                f"check_mingw_import_thunks: FAIL {rt_path}: "
                "runtime archive not found or unreadable",
                file=sys.stderr,
            )
            ok = False
            continue
        rc, stdout, stderr = runner(tool, rt_path)
        if rc != 0:
            detail = stderr.strip() or stdout.strip() or f"exit {rc}"
            print(
                f"check_mingw_import_thunks: FAIL {rt_path}: "
                f"nm ({tool}) failed on runtime archive (exit {rc}): {detail}",
                file=sys.stderr,
            )
            ok = False
            continue
        results.append((rt_path, parse_nm_defined_globals(stdout)))
    return results, ok


def _check_single_archive(
    archive: Path,
    target_dll_dir: Path,
    tool: ToolSpec,
    runner: Runner,
    dll_reader: DllExportsReader,
    runtime_data: Sequence[tuple[Path, frozenset[str]]] = (),
    all_dll_reader: DllExportsReader = read_dll_all_exports,
) -> bool:
    dll_path = find_paired_dll(archive, target_dll_dir)
    if dll_path is None:
        stem = archive.name[: -len(".dll.a")]
        print(
            f"check_mingw_import_thunks: FAIL {archive}: "
            f"no paired DLL ({stem}.dll or {stem}-*.dll) in {target_dll_dir}",
            file=sys.stderr,
        )
        return False
    try:
        func_exports = dll_reader(dll_path)
    except (OSError, ValueError) as exc:
        print(
            f"check_mingw_import_thunks: FAIL {archive} ({dll_path.name}): "
            f"cannot read DLL exports: {exc}",
            file=sys.stderr,
        )
        return False
    rc, stdout, stderr = runner(tool, archive)
    if rc != 0:
        detail = stderr.strip() or stdout.strip() or f"exit {rc}"
        print(
            f"check_mingw_import_thunks: {archive}: nm ({tool}) failed (exit {rc}): {detail}",
            file=sys.stderr,
        )
        return False
    thunk_symbols = parse_nm_thunks(stdout)
    if not func_exports:
        print(
            f"check_mingw_import_thunks: FAIL {archive} ({dll_path.name}): "
            f"func_exports=0, thunks={len(thunk_symbols)}, missing=0 "
            "(DLL yields zero function exports)",
            file=sys.stderr,
        )
        return False
    thunk_ok = _report_thunk_coverage(archive, dll_path, func_exports, thunk_symbols)
    runtime_ok = True
    if runtime_data:
        runtime_ok = _check_runtime_overlap(dll_path, runtime_data, all_dll_reader)
    return thunk_ok and runtime_ok


def _report_thunk_coverage(
    archive: Path, dll_path: Path, func_exports: frozenset[str], thunk_symbols: frozenset[str]
) -> bool:
    """Print the per-archive verdict; return True when every function export has a thunk."""
    missing = sorted(func_exports - thunk_symbols)
    present = len(func_exports) - len(missing)
    if not missing:
        print(
            f"check_mingw_import_thunks: OK {archive} ({dll_path.name}): "
            f"func_exports={len(func_exports)}, thunks={present}, missing=0"
        )
        return True
    preview = ", ".join(missing[:MAX_MISSING_PREVIEW])
    extra = (
        f" (+{len(missing) - MAX_MISSING_PREVIEW} more)"
        if len(missing) > MAX_MISSING_PREVIEW
        else ""
    )
    print(
        f"check_mingw_import_thunks: FAIL {archive} ({dll_path.name}): "
        f"func_exports={len(func_exports)}, thunks={present}, missing={len(missing)} "
        f"[{preview}{extra}] (LTO-linked DLL import library, VMAFx/vmafx#3257, ADR-3397)",
        file=sys.stderr,
    )
    return False


def check_build_dir(
    build_dir: Path,
    nm_tool: ToolSpec | None = None,
    runner: Runner = default_run_nm,
    dll_dir: Path | None = None,
    dll_reader: DllExportsReader = read_dll_function_exports,
    runtime_archives: Sequence[Path | str] = (),
    runtime_runner: Runner = default_run_nm_defined_globals,
    all_dll_reader: DllExportsReader = read_dll_all_exports,
) -> int:
    """Inspect ``<build_dir>/src/*.dll.a`` against paired DLLs; return 0 on pass, 1 on failure."""
    src_dir = build_dir / "src"
    archives = sorted(src_dir.glob("*.dll.a")) if src_dir.is_dir() else []
    if not archives:
        print(
            f"check_mingw_import_thunks: {src_dir}: no *.dll.a import libraries found",
            file=sys.stderr,
        )
        return 1

    target_dll_dir = dll_dir if dll_dir is not None else src_dir
    tool: ToolSpec = nm_tool or os.environ.get("NM") or "nm"

    runtime_data: list[tuple[Path, frozenset[str]]] = []
    if runtime_archives:
        norm_paths = [Path(p) for p in runtime_archives]
        runtime_data, ok = _load_runtime_archives(norm_paths, tool, runtime_runner)
        if not ok:
            return 1

    failed = False
    for archive in archives:
        if not _check_single_archive(
            archive,
            target_dll_dir,
            tool,
            runner,
            dll_reader,
            runtime_data=runtime_data,
            all_dll_reader=all_dll_reader,
        ):
            failed = True
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "build_dir",
        type=Path,
        help="Meson build directory containing src/*.dll.a import libraries",
    )
    parser.add_argument(
        "--dll-dir",
        type=Path,
        default=None,
        help="Directory containing paired *.dll files (default: <build_dir>/src)",
    )
    parser.add_argument(
        "--nm",
        default=None,
        help="nm binary or command prefix to use (default: $NM or nm)",
    )
    parser.add_argument(
        "--runtime-archive",
        action="append",
        type=Path,
        default=[],
        help=(
            "Path to toolchain static runtime archive (e.g. libgcc_eh.a) "
            "to check for exported symbols (repeatable)"
        ),
    )
    args = parser.parse_args(argv)
    return check_build_dir(
        args.build_dir,
        nm_tool=args.nm,
        dll_dir=args.dll_dir,
        runtime_archives=args.runtime_archive,
    )


if __name__ == "__main__":
    sys.exit(main())
