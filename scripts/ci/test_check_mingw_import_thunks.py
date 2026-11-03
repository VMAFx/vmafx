#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for ``scripts/ci/check_mingw_import_thunks.py`` (ADR-3397, #3257)."""

from __future__ import annotations

import importlib.util
import io
import struct
import sys
import tempfile
from collections.abc import Sequence
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_mingw_import_thunks",
    Path(__file__).with_name("check_mingw_import_thunks.py"),
)
assert _SPEC and _SPEC.loader
gate = importlib.util.module_from_spec(_SPEC)
sys.modules["check_mingw_import_thunks"] = gate
_SPEC.loader.exec_module(gate)

TWO_FUNCS = ("vmaf_picture_alloc", "vmaf_write_output_with_format")

WITH_THUNKS = """\
libvmaf_3_dll_d000078.o:
                 U _head_libvmaf_3_dll
0000000000000000 i .idata$4
0000000000000000 I __imp_vmaf_write_output_with_format
0000000000000000 t .text
0000000000000000 T vmaf_write_output_with_format

libvmaf_3_dll_d000042.o:
0000000000000000 I __imp_vmaf_picture_alloc
0000000000000000 t .text
0000000000000000 T vmaf_picture_alloc
"""

PLANTED_LTO_NO_THUNKS = """\
libvmaf_3_dll_d000078.o:
                 U _head_libvmaf_3_dll
0000000000000000 i .idata$4
0000000000000000 I __imp_vmaf_write_output_with_format
0000000000000000 I __nm_vmaf_write_output_with_format
0000000000000000 t .text

libvmaf_3_dll_d000042.o:
0000000000000000 I __imp_vmaf_picture_alloc
0000000000000000 I __nm_vmaf_picture_alloc
0000000000000000 t .text
"""

# Measured on MinGW GCC 16.2.0 + GNU ld 2.47 (build/mingw-measure/{nolto,lto}/src):
NOLTO_VMAF_FUNCS = 78
NOLTO_VMAFX_FUNCS = 1015
NOLTO_VMAFX_DATA = 119
LTO_VMAF_FUNCS = 0
LTO_VMAFX_FUNCS = 52
LTO_VMAFX_THUNKS = 48
LTO_VMAFX_MISSING = (
    "vif_compute_line_residuals",
    "vmaf_ceiln",
    "vmaf_floorn",
    "vmaf_log",
)


def _pack_edata_section(
    all_symbols: list[tuple[str, bool]], edata_rva: int
) -> tuple[bytes, int, int]:
    count = len(all_symbols)
    addr_off = 40
    name_ptr_off = addr_off + count * 4
    ord_off = name_ptr_off + count * 4
    strings_off = ord_off + count * 2
    str_blob = bytearray(b"test.dll\x00")
    dll_name_rva = edata_rva + strings_off
    addrs: list[int] = []
    name_rvas: list[int] = []
    for idx, (sym, is_func) in enumerate(all_symbols):
        base_rva = 0x1000 if is_func else 0x3000
        addrs.append(base_rva + (idx % 2048) * 4)
        name_rvas.append(edata_rva + strings_off + len(str_blob))
        str_blob.extend(sym.encode("ascii") + b"\x00")
    edata = bytearray(
        struct.pack(
            "<IIHHIIIIIII",
            0,
            0,
            0,
            0,
            dll_name_rva,
            1,
            count,
            count,
            edata_rva + addr_off,
            edata_rva + name_ptr_off,
            edata_rva + ord_off,
        )
    )
    for addr in addrs:
        edata.extend(struct.pack("<I", addr))
    for name_rva in name_rvas:
        edata.extend(struct.pack("<I", name_rva))
    for idx in range(count):
        edata.extend(struct.pack("<H", idx))
    edata.extend(str_blob)
    edata_size = len(edata)
    raw_size = ((max(edata_size, 1) + 0x1FF) // 0x200) * 0x200
    edata.extend(b"\x00" * (raw_size - edata_size))
    return bytes(edata), edata_size, raw_size


def _build_pe_dll_bytes(
    func_symbols: Sequence[str] = (),
    data_symbols: Sequence[str] = (),
) -> bytes:
    """Build a minimal PE32+ DLL image with ``.text`` (CODE), ``.data``, and ``.edata``."""
    all_symbols = sorted(
        [(name, True) for name in func_symbols] + [(name, False) for name in data_symbols]
    )
    edata_rva = 0x5000
    edata_bytes, edata_size, edata_raw = _pack_edata_section(all_symbols, edata_rva)
    buf = bytearray(0x600)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x40)
    buf[0x40:0x44] = b"PE\x00\x00"
    struct.pack_into("<HHIIIHH", buf, 0x44, 0x8664, 3, 0, 0, 0, 240, 0x2022)
    opt_off = 0x58
    struct.pack_into("<H", buf, opt_off, 0x20B)
    struct.pack_into("<Q", buf, opt_off + 24, 0x180000000)
    struct.pack_into("<II", buf, opt_off + 32, 0x1000, 0x200)
    struct.pack_into("<II", buf, opt_off + 56, 0x5000 + edata_raw, 0x200)
    struct.pack_into("<I", buf, opt_off + 108, 16)
    if all_symbols:
        struct.pack_into("<II", buf, opt_off + 112, edata_rva, edata_size)
    sec_defs = (
        (b".text\x00\x00\x00", 0x2000, 0x1000, 0x200, 0x200, 0x60000020),
        (b".data\x00\x00\x00", 0x2000, 0x3000, 0x200, 0x400, 0xC0000040),
        (b".edata\x00\x00", edata_size, edata_rva, edata_raw, 0x600, 0x40000040),
    )
    for idx, (sname, vsize, vrva, rsize, rptr, chars) in enumerate(sec_defs):
        struct.pack_into(
            "<8sIIIIIIHHI",
            buf,
            opt_off + 240 + idx * 40,
            sname,
            vsize,
            vrva,
            rsize,
            rptr,
            0,
            0,
            0,
            0,
            chars,
        )
    return bytes(buf) + edata_bytes


def _generate_nm_output(imp_symbols: Sequence[str], thunk_symbols: Sequence[str]) -> str:
    lines = [f"0000000000000000 I __imp_{name}" for name in imp_symbols]
    lines.extend([f"0000000000000000 T {name}" for name in thunk_symbols])
    return "\n".join(lines) + "\n"


def _make_build_dir(
    root: Path,
    archives: dict[str, tuple[Sequence[str], Sequence[str]]] | Sequence[str],
) -> Path:
    src = root / "src"
    src.mkdir(parents=True, exist_ok=True)
    items = archives if isinstance(archives, dict) else dict.fromkeys(archives, (TWO_FUNCS, ()))
    for ar_name, (funcs, datas) in items.items():
        (src / ar_name).write_bytes(b"!<arch>\n")
        stem = ar_name[: -len(".dll.a")]
        dll_name = f"{stem}-3.dll" if stem == "libvmaf" else f"{stem}-1.dll"
        (src / dll_name).write_bytes(_build_pe_dll_bytes(funcs, datas))
    return root


def test_positive_thunks_present_passes() -> None:
    """Positive: every DLL function export has a T thunk in its .dll.a and exits 0."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmaf.dll.a", "libvmafx.dll.a"))
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, WITH_THUNKS, ""),
            )
        assert rc == 0, err.getvalue()
        assert err.getvalue() == ""
        lines = out.getvalue().strip().splitlines()
        assert len(lines) == len(("libvmaf.dll.a", "libvmafx.dll.a"))
        assert "OK" in lines[0] and "func_exports=2, thunks=2, missing=0" in lines[0]
        assert "OK" in lines[1] and "func_exports=2, thunks=2, missing=0" in lines[1]


def test_negative_missing_single_thunk_among_many_fails() -> None:
    """Negative (T2): 1 missing function thunk out of 10 fails even though ratio is 0.9."""
    with tempfile.TemporaryDirectory() as tmp:
        funcs = tuple(f"sym_{i}" for i in range(10))
        thunks = funcs[:-1]
        build = _make_build_dir(Path(tmp), {"libvmaf.dll.a": (funcs, ())})
        nm_out = _generate_nm_output(funcs, thunks)
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, nm_out, ""),
            )
        assert rc == 1
        assert "libvmaf.dll.a" in err.getvalue()
        assert "func_exports=10, thunks=9, missing=1" in err.getvalue()
        assert "sym_9" in err.getvalue()


def test_positive_data_exports_excluded_from_denominator_passes() -> None:
    """Positive (T5): a DLL with more data exports than functions passes when functions have thunks."""
    with tempfile.TemporaryDirectory() as tmp:
        funcs = ("fn_0", "fn_1")
        datas = tuple(f"data_{i}" for i in range(8))
        build = _make_build_dir(Path(tmp), {"libvmaf.dll.a": (funcs, datas)})
        nm_out = _generate_nm_output((*funcs, *datas), funcs)
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, nm_out, ""),
            )
        assert rc == 0, err.getvalue()
        assert "func_exports=2, thunks=2, missing=0" in out.getvalue()


def test_negative_planted_only_imp_no_thunks_fails() -> None:
    """Negative (planted): function exports with zero T thunks fail naming missing functions."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmaf.dll.a", "libvmafx.dll.a"))
        outputs = {
            "libvmaf.dll.a": PLANTED_LTO_NO_THUNKS,
            "libvmafx.dll.a": WITH_THUNKS,
        }
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, path: (0, outputs[path.name], ""),
            )
        assert rc == 1
        assert "libvmaf.dll.a" in err.getvalue()
        assert "func_exports=2, thunks=0, missing=2" in err.getvalue()
        assert "vmaf_picture_alloc" in err.getvalue()
        assert "libvmafx.dll.a" in out.getvalue()


def test_boundary_no_dll_a_found_fails() -> None:
    """Boundary: a build dir with no *.dll.a under src/ fails closed with exit 1."""
    with tempfile.TemporaryDirectory() as tmp:
        empty_build = Path(tmp) / "empty"
        (empty_build / "src").mkdir(parents=True)
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.main([str(empty_build)])
        assert rc == 1
        assert "no *.dll.a import libraries found" in err.getvalue()


def test_boundary_missing_paired_dll_fails() -> None:
    """Boundary: a *.dll.a without a paired DLL in src/ fails closed with exit 1."""
    with tempfile.TemporaryDirectory() as tmp:
        build = Path(tmp)
        src = build / "src"
        src.mkdir(parents=True)
        (src / "libvmaf.dll.a").write_bytes(b"!<arch>\n")
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, WITH_THUNKS, ""),
            )
        assert rc == 1
        assert "no paired DLL" in err.getvalue()


def test_boundary_nm_failure_nonzero_fails() -> None:
    """Boundary: a non-zero exit status from nm fails closed with exit 1."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), ("libvmaf.dll.a",))
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="fake-nm",
                runner=lambda _tool, _path: (2, "", "file format not recognized"),
            )
        assert rc == 1
        assert "libvmaf.dll.a" in err.getvalue()
        assert "exit 2" in err.getvalue()
        assert "file format not recognized" in err.getvalue()


def test_fake_nm_script_end_to_end() -> None:
    """End-to-end CLI invocation with a cross-platform Python fake nm script (T6)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        build = _make_build_dir(tmp_path, ("libvmaf.dll.a",))
        fake_nm = tmp_path / "fake_nm.py"
        fake_nm.write_text(
            f"import sys\nsys.stdout.write({PLANTED_LTO_NO_THUNKS!r})\n",
            encoding="utf-8",
        )
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.main(
                [
                    "--nm",
                    f'"{sys.executable}" "{fake_nm}"',
                    "--dll-dir",
                    str(build / "src"),
                    str(build),
                ]
            )
        assert rc == 1
        assert "func_exports=2, thunks=0, missing=2" in err.getvalue()


def test_zero_function_exports_fails() -> None:
    """Boundary (T4/T10): a DLL yielding 0 function exports (or empty archive) fails closed."""
    with tempfile.TemporaryDirectory() as tmp:
        build = _make_build_dir(Path(tmp), {"libvmaf.dll.a": ((), ("only_data",))})
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, "", ""),
            )
        assert rc == 1
        assert "func_exports=0" in err.getvalue()


def test_measured_mingw_nolto_and_lto_counts() -> None:
    """Measured MinGW GCC 16.2 counts: nolto (78/78, 1015/1015) passes; lto (0/0, 48/52) fails."""
    with tempfile.TemporaryDirectory() as tmp:
        nolto = Path(tmp) / "nolto"
        vmaf_funcs = tuple(f"vmaf_fn_{i}" for i in range(NOLTO_VMAF_FUNCS))
        vmafx_funcs = tuple(f"vmafx_fn_{i}" for i in range(NOLTO_VMAFX_FUNCS))
        vmafx_datas = tuple(f"vmafx_data_{i}" for i in range(NOLTO_VMAFX_DATA))
        _make_build_dir(
            nolto,
            {
                "libvmaf.dll.a": (vmaf_funcs, ()),
                "libvmafx.dll.a": (vmafx_funcs, vmafx_datas),
            },
        )
        nolto_nm = {
            "libvmaf.dll.a": _generate_nm_output(vmaf_funcs, vmaf_funcs),
            "libvmafx.dll.a": _generate_nm_output((*vmafx_funcs, *vmafx_datas), vmafx_funcs),
        }
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc_nolto = gate.check_build_dir(
                nolto,
                nm_tool="nm",
                runner=lambda _tool, path: (0, nolto_nm[path.name], ""),
            )
        assert rc_nolto == 0, err.getvalue()
        assert "func_exports=78, thunks=78, missing=0" in out.getvalue()
        assert "func_exports=1015, thunks=1015, missing=0" in out.getvalue()

        lto = Path(tmp) / "lto"
        lto_avx512 = tuple(f"avx512_fn_{i}" for i in range(LTO_VMAFX_THUNKS))
        lto_vmafx_funcs = (*lto_avx512, *LTO_VMAFX_MISSING)
        assert len(lto_vmafx_funcs) == LTO_VMAFX_FUNCS
        _make_build_dir(
            lto,
            {
                "libvmaf.dll.a": ((), ()),
                "libvmafx.dll.a": (lto_vmafx_funcs, ()),
            },
        )
        lto_nm = {
            "libvmaf.dll.a": _generate_nm_output(vmaf_funcs, ()),
            "libvmafx.dll.a": _generate_nm_output(lto_vmafx_funcs, lto_avx512),
        }
        err_lto = io.StringIO()
        with redirect_stderr(err_lto):
            rc_lto = gate.check_build_dir(
                lto,
                nm_tool="nm",
                runner=lambda _tool, path: (0, lto_nm[path.name], ""),
            )
        assert rc_lto == 1
        assert "libvmaf.dll.a" in err_lto.getvalue() and "func_exports=0" in err_lto.getvalue()
        assert "func_exports=52, thunks=48, missing=4" in err_lto.getvalue()
        for sym in LTO_VMAFX_MISSING:
            assert sym in err_lto.getvalue()


def test_positive_strip_surrounding_quotes_matching() -> None:
    """Positive: matching double or single quotes are stripped from tokens."""
    assert gate.strip_surrounding_quotes('"C:\\py.exe"') == "C:\\py.exe"
    assert gate.strip_surrounding_quotes("'C:\\fake.py'") == "C:\\fake.py"
    assert gate.strip_surrounding_quotes('"hello world"') == "hello world"
    assert gate.strip_surrounding_quotes("'hello world'") == "hello world"


def test_negative_strip_surrounding_quotes_unmatched() -> None:
    """Negative: unmatched surrounding quotes are kept unchanged."""
    assert gate.strip_surrounding_quotes('"unmatched') == '"unmatched'
    assert gate.strip_surrounding_quotes('unmatched"') == 'unmatched"'
    assert gate.strip_surrounding_quotes("'unmatched") == "'unmatched"
    assert gate.strip_surrounding_quotes("unmatched'") == "unmatched'"
    assert gate.strip_surrounding_quotes("\"mismatch'") == "\"mismatch'"
    assert gate.strip_surrounding_quotes("'mismatch\"") == "'mismatch\""
    assert gate.strip_surrounding_quotes("plain") == "plain"


def test_boundary_strip_surrounding_quotes() -> None:
    """Boundary: empty quotes yield empty string; single quote char is kept."""
    assert gate.strip_surrounding_quotes('""') == ""
    assert gate.strip_surrounding_quotes("''") == ""
    assert gate.strip_surrounding_quotes('"') == '"'
    assert gate.strip_surrounding_quotes("'") == "'"
    assert gate.strip_surrounding_quotes("") == ""


def test_positive_runtime_archive_disjoint_passes() -> None:
    """Positive: runtime archive defined symbols are disjoint from DLL exports and exits 0."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        build = _make_build_dir(tmp_path, ("libvmaf.dll.a",))
        rt_archive = tmp_path / "libgcc_eh.a"
        rt_archive.write_bytes(b"!<arch>\n")
        rt_nm_out = "0000000000000000 T other_runtime_symbol\n"
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, WITH_THUNKS, ""),
                runtime_archives=(rt_archive,),
                runtime_runner=lambda _tool, _path: (0, rt_nm_out, ""),
            )
        assert rc == 0, err.getvalue()
        assert err.getvalue() == ""
        assert "runtime_overlap=0" in out.getvalue()
        assert "libgcc_eh.a" in out.getvalue()


def test_negative_planted_runtime_archive_function_overlap_fails() -> None:
    """Negative (planted): DLL exports _Unwind_Resume defined in runtime archive -> 1."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        funcs = ("_Unwind_Resume", "vmaf_picture_alloc")
        build = _make_build_dir(tmp_path, {"libvmafx.dll.a": (funcs, ())})
        rt_archive = tmp_path / "libgcc_eh.a"
        rt_archive.write_bytes(b"!<arch>\n")
        thunk_nm_out = _generate_nm_output(funcs, funcs)
        rt_nm_out = "0000000000000410 T _Unwind_Resume\n"
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, thunk_nm_out, ""),
                runtime_archives=(rt_archive,),
                runtime_runner=lambda _tool, _path: (0, rt_nm_out, ""),
            )
        assert rc == 1
        assert "_Unwind_Resume" in err.getvalue()
        assert "FAIL" in err.getvalue()
        assert "libgcc_eh.a" in err.getvalue()
        assert "-Wl,--exclude-libs" in err.getvalue()


def test_negative_runtime_archive_weak_defined_symbol_fails() -> None:
    """Negative: runtime nm reports _Unwind_Resume as weak defined W and DLL exports it -> 1."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        funcs = ("_Unwind_Resume", "vmaf_picture_alloc")
        build = _make_build_dir(tmp_path, {"libvmafx.dll.a": (funcs, ())})
        rt_archive = tmp_path / "libgcc_eh.a"
        rt_archive.write_bytes(b"!<arch>\n")
        thunk_nm_out = _generate_nm_output(funcs, funcs)
        rt_nm_out = "0000000000000410 W _Unwind_Resume\n"
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, thunk_nm_out, ""),
                runtime_archives=(rt_archive,),
                runtime_runner=lambda _tool, _path: (0, rt_nm_out, ""),
            )
        assert rc == 1
        assert "_Unwind_Resume" in err.getvalue()
        assert "FAIL" in err.getvalue()
        assert "libgcc_eh.a" in err.getvalue()


def test_negative_runtime_archive_data_overlap_fails() -> None:
    """Negative: overlap through a DATA export fails closed with exit 1."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        funcs = ("vmaf_picture_alloc",)
        datas = ("_Unwind_Custom_Data",)
        build = _make_build_dir(tmp_path, {"libvmaf.dll.a": (funcs, datas)})
        rt_archive = tmp_path / "libgcc_eh.a"
        rt_archive.write_bytes(b"!<arch>\n")
        thunk_nm_out = _generate_nm_output(funcs, funcs)
        rt_nm_out = "0000000000000010 D _Unwind_Custom_Data\n"
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, thunk_nm_out, ""),
                runtime_archives=(rt_archive,),
                runtime_runner=lambda _tool, _path: (0, rt_nm_out, ""),
            )
        assert rc == 1
        assert "_Unwind_Custom_Data" in err.getvalue()
        assert "FAIL" in err.getvalue()
        assert "libgcc_eh.a" in err.getvalue()


def test_boundary_runtime_archive_no_defined_globals_passes() -> None:
    """Boundary: runtime nm output with only U / lower-case / I lines passes."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        build = _make_build_dir(tmp_path, ("libvmaf.dll.a",))
        rt_archive = tmp_path / "libempty.a"
        rt_archive.write_bytes(b"!<arch>\n")
        rt_nm_out = (
            "                 U external_sym\n"
            "0000000000000000 t local_func\n"
            "0000000000000000 d local_data\n"
            "0000000000000000 I __imp_vmaf_picture_alloc\n"
            "0000000000000000 w weak_sym\n"
        )
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, WITH_THUNKS, ""),
                runtime_archives=(rt_archive,),
                runtime_runner=lambda _tool, _path: (0, rt_nm_out, ""),
            )
        assert rc == 0, err.getvalue()
        assert "runtime_overlap=0" in out.getvalue()


def test_boundary_runtime_archive_missing_path_fails() -> None:
    """Boundary: missing runtime archive path fails closed with exit 1."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        build = _make_build_dir(tmp_path, ("libvmaf.dll.a",))
        missing_archive = tmp_path / "nonexistent_archive.a"
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, WITH_THUNKS, ""),
                runtime_archives=(missing_archive,),
            )
        assert rc == 1
        assert "nonexistent_archive.a" in err.getvalue()
        assert "FAIL" in err.getvalue()


def test_boundary_runtime_archive_nm_nonzero_fails() -> None:
    """Boundary: nm returning non-zero on runtime archive fails closed with exit 1."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        build = _make_build_dir(tmp_path, ("libvmaf.dll.a",))
        rt_archive = tmp_path / "libcorrupt.a"
        rt_archive.write_bytes(b"!<arch>\n")
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.check_build_dir(
                build,
                nm_tool="nm",
                runner=lambda _tool, _path: (0, WITH_THUNKS, ""),
                runtime_archives=(rt_archive,),
                runtime_runner=lambda _tool, _path: (2, "", "archive format error"),
            )
        assert rc == 1
        assert "libcorrupt.a" in err.getvalue()
        assert "exit 2" in err.getvalue()
        assert "archive format error" in err.getvalue()


def test_parse_nm_defined_globals() -> None:
    """Parse test for nm defined global symbols: allow-list kept, U/w/v/lower ignored."""
    nm_input = """\
archive.o:
0000000000000140 T _GCC_specific_handler
0000000000000010 D global_data_var
0000000000000020 B global_bss_var
0000000000000030 R global_rodata_var
0000000000000040 S global_small_data
0000000000000050 C common_symbol
0000000000000060 G global_small_init_data
0000000000000000 w weak_func
0000000000000000 W weak_func_upper
0000000000000000 v weak_var
0000000000000000 V weak_var_upper
                 U undefined_sym
0000000000000000 U undefined_with_addr
0000000000000000 I __imp_foo
0000000000000000 t local_text
0000000000000000 d local_data
0000000000000000 b local_bss
0000000000000000 r local_rodata
"""
    expected = frozenset(
        {
            "_GCC_specific_handler",
            "global_data_var",
            "global_bss_var",
            "global_rodata_var",
            "global_small_data",
            "common_symbol",
            "global_small_init_data",
            "weak_func_upper",
            "weak_var_upper",
        }
    )
    assert gate.parse_nm_defined_globals(nm_input) == expected


def test_read_dll_all_exports_includes_data_and_code() -> None:
    """Verify read_dll_function_exports returns CODE only, read_dll_all_exports returns both."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        dll_path = tmp_path / "test.dll"
        dll_bytes = _build_pe_dll_bytes(
            func_symbols=("func_a", "func_b"),
            data_symbols=("data_x", "data_y"),
        )
        dll_path.write_bytes(dll_bytes)
        funcs = gate.read_dll_function_exports(dll_path)
        all_syms = gate.read_dll_all_exports(dll_path)
        assert funcs == frozenset({"func_a", "func_b"})
        assert all_syms == frozenset({"func_a", "func_b", "data_x", "data_y"})


def test_cli_runtime_archive_end_to_end() -> None:
    """End-to-end CLI with --runtime-archive option."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        funcs = ("_Unwind_Resume",)
        build = _make_build_dir(tmp_path, {"libvmaf.dll.a": (funcs, ())})
        rt_archive = tmp_path / "libgcc_eh.a"
        rt_archive.write_bytes(b"!<arch>\n")
        fake_nm = tmp_path / "fake_nm.py"
        fake_nm.write_text(
            f"""\
import sys
if "--defined-only" in sys.argv:
    sys.stdout.write("0000000000000000 T _Unwind_Resume\\n")
else:
    sys.stdout.write({_generate_nm_output(funcs, funcs)!r})
""",
            encoding="utf-8",
        )
        err = io.StringIO()
        with redirect_stderr(err):
            rc = gate.main(
                [
                    "--nm",
                    f'"{sys.executable}" "{fake_nm}"',
                    "--dll-dir",
                    str(build / "src"),
                    "--runtime-archive",
                    str(rt_archive),
                    str(build),
                ]
            )
        assert rc == 1
        assert "_Unwind_Resume" in err.getvalue()
        assert "libgcc_eh.a" in err.getvalue()
