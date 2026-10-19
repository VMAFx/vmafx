# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the Windows tester zip (ADR-1515), on Linux: host facts, report wording,
the launcher, the PE import check and the build script's own steps."""

from __future__ import annotations

import importlib.util
import json
import struct
import sys
import zipfile
import zlib
from pathlib import Path

import pytest
from zopfli import zopfli

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[2]
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester import hw_facts, hw_report, hw_winfacts


def load_script(name: str):
    path = _REPO / "scripts" / "ci" / name
    spec = importlib.util.spec_from_file_location(name.replace("-", "_").removesuffix(".py"), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


imports_check = load_script("check-windows-bundle-imports.py")
builder = load_script("build-windows-tester-bundle.py")
SCHEMA = json.loads((_REPO / "docs/hardware-reports/report.schema.json").read_text())

X86_REGISTRY = {
    "ProcessorNameString": "13th Gen Intel(R) Core(TM) i9-13900K  ",
    "VendorIdentifier": "GenuineIntel",
    "Identifier": "Intel64 Family 6 Model 183 Stepping 1",
}
ARM_REGISTRY = {
    "ProcessorNameString": "Snapdragon(R) X Elite - X1E78100 - Qualcomm(R) Oryon(TM) CPU",
    "VendorIdentifier": "Qualcomm Technologies Inc",
    "Identifier": "ARMv8 (64-bit) Family 8 Model 1 Revision 201",
}


def probe(numbers: set[int]):
    return lambda number: number in numbers


# ---- host facts ------------------------------------------------------------------------


def test_windows_machine_names_are_the_linux_ones() -> None:
    assert hw_winfacts.windows_machine("AMD64") == "x86_64"
    assert hw_winfacts.windows_machine("ARM64") == "aarch64"
    assert hw_winfacts.windows_machine("x86") == "x86"


def test_cpu_registry_values_map_to_allow_listed_keys() -> None:
    x86 = hw_winfacts.read_cpu("x86_64", X86_REGISTRY.get)
    assert x86 == {"model name": "13th Gen Intel(R) Core(TM) i9-13900K", "vendor_id": "GenuineIntel",
                   "cpu family": "6", "model": "183", "stepping": "1"}  # fmt: skip
    arm = hw_winfacts.read_cpu("aarch64", ARM_REGISTRY.get)
    assert arm["cpu architecture"] == "8" and arm["cpu part"] == "1"
    assert arm["cpu revision"] == "201"
    assert set(x86) | set(arm) <= hw_facts.ALLOWED_CPUINFO_KEYS
    assert hw_winfacts.read_cpu("x86_64", {}.get) == {}  # no registry: nothing invented


def test_x86_dispatch_follows_the_cpu_c_levels() -> None:
    every = set(hw_winfacts.X86_FEATURES.values())
    found = hw_winfacts.features("x86_64", probe(every))
    assert hw_winfacts.dispatch_flags("x86_64", found) == ["sse2", "ssse3", "sse4.1", "avx2",
                                                           "avx512"]  # fmt: skip
    no_sse3 = hw_winfacts.features("x86_64", probe(every - {13}))
    assert hw_winfacts.dispatch_flags("x86_64", no_sse3) == ["sse2", "avx2", "avx512"]
    no_avx2 = hw_winfacts.features("x86_64", probe(every - {40}))
    assert "avx2" not in hw_winfacts.dispatch_flags("x86_64", no_avx2)
    assert "avx512" not in hw_winfacts.dispatch_flags("x86_64", no_avx2)  # AVX-512 needs AVX2
    assert hw_winfacts.dispatch_flags("x86_64", []) == []


def test_arm64_dispatch_is_neon_only() -> None:
    found = hw_winfacts.features("aarch64", probe({47}))
    assert found == ["sve2"]  # reported as a feature, never dispatched by the MSVC build
    assert hw_winfacts.dispatch_flags("aarch64", found) == ["neon"]


@pytest.mark.parametrize(("raw", "registry"), [("AMD64", X86_REGISTRY), ("ARM64", ARM_REGISTRY)])
def test_windows_facts_fit_the_schema_and_carry_no_identifier(monkeypatch, raw, registry) -> None:
    monkeypatch.setattr(hw_winfacts.platform, "machine", lambda: raw)
    monkeypatch.setattr(hw_winfacts.platform, "release", lambda: "11")
    monkeypatch.setattr(hw_winfacts.platform, "version", lambda: "10.0.26100")
    facts = hw_winfacts.collect_windows_facts(registry.get, probe({10, 13, 36, 37, 39, 40}))
    host = SCHEMA["properties"]["host"]
    assert set(host["required"]) <= set(facts) <= set(host["properties"])
    assert facts["platform"] in host["properties"]["platform"]["enum"]
    assert facts["os_version"] == "Windows 11 (10.0.26100)"
    assert facts["machine"] == hw_winfacts.windows_machine(raw)
    text = json.dumps(facts).lower()
    assert "hostname" not in text and "serial" not in text and "uuid" not in text


def test_vmaf_binary_prefers_the_windows_executable(tmp_path: Path) -> None:
    tools = tmp_path / "build" / "tools"
    tools.mkdir(parents=True)
    assert hw_facts.vmaf_binary(tmp_path) == tools / "vmaf"
    (tools / "vmaf.exe").write_bytes(b"MZ")
    assert hw_facts.vmaf_binary(tmp_path) == tools / "vmaf.exe"


# ---- report wording, file name, output file --------------------------------------------


def windows_report(machine: str, flags: list[str]) -> dict:
    host = {"platform": "windows", "machine": machine, "dispatch_flags": flags}
    return {"host": host, "metal_equivalence": {"status": "not_applicable"},
            "image": {"kind": "windows-zip", "gpu_backend": None}}  # fmt: skip


def test_windows_not_exercised_names_metal_gpu_and_the_cpu_gaps() -> None:
    na = {"golden": "not in the Windows zip"}
    x64 = {e["item"]: e["reason"] for e in hw_report.not_exercised(
        windows_report("x86_64", ["sse2", "avx2"]), [], na)}  # fmt: skip
    assert "Windows zip" in x64["Metal"] and "MSVC" in x64["CUDA, SYCL and HIP twins"]
    assert "AVX-512 kernels" in x64 and "golden check" in x64
    arm = {e["item"]: e["reason"] for e in hw_report.not_exercised(
        windows_report("aarch64", ["neon"]), [], na)}  # fmt: skip
    assert "ADR-1260" in arm["SVE2 kernels"] and "x86 kernels" in arm


def test_a_windows_report_gets_its_own_file_name() -> None:
    report = {"image": {"kind": "windows-zip", "gpu_backend": None},
              "host": {"cpu_model": "AMD Ryzen 7 7840U"}, "generated_utc": "2026-10-04T10:00:00Z"}  # fmt: skip
    name = hw_report.suggested_file_name(report)
    assert name == "docs/hardware-reports/2026-10-04-amd-ryzen-7-7840u-windows.json"
    report["image"]["kind"] = "container-image"
    assert hw_report.suggested_file_name(report).endswith("7840u.json")


def test_output_writes_the_report_as_utf8_and_keeps_stdout_empty(tmp_path, capsys) -> None:
    image = tmp_path / "image-root"
    (image / "image").mkdir(parents=True)
    (image / "image" / "fixtures.json").write_text('{"fixtures": []}')
    out = tmp_path / "report.json"
    args = ["--image-root", str(image), "--output", str(out)]
    for check in hw_report.CHECKS:
        args += ["--skip", check]
    assert hw_report.main(args) == 1  # files_match_build is false without build info
    captured = capsys.readouterr()
    assert captured.out == "" and f"report written to {out}" in captured.err
    report = json.loads(out.read_bytes().decode("utf-8"))
    assert report["schema_version"] == hw_report.SCHEMA_VERSION


# ---- the launcher ------------------------------------------------------------------------


def test_run_cmd_checks_the_architecture_and_writes_report_json() -> None:
    text = (_REPO / "tools/rc1-tester/image/windows/run.cmd").read_text()
    assert "image\\package-arch.txt" in text and "%PROCESSOR_ARCHITECTURE%" in text
    assert "exit /b 64" in text
    assert "-I -B -X utf8" in text and "--output report.json %*" in text
    # LF-only batch files misparse labels; the launcher must not need one.
    assert not [line for line in text.splitlines() if line.lstrip().startswith(":")]


# ---- PE import check ---------------------------------------------------------------------


def make_pe(machine: int, names: list[str], delay: list[str] = ()) -> bytes:
    """A minimal PE32+ image with one section holding its import tables."""
    section_rva, section_raw = 0x1000, 0x200
    blob = bytearray()
    imports_at = 0
    blob += bytes(20 * (len(names) + 1))
    delay_at = len(blob)
    blob += bytes(32 * (len(delay) + 1))
    for index, name in enumerate(names):
        struct.pack_into("<I", blob, imports_at + 20 * index + 12, section_rva + len(blob))
        blob += name.encode() + b"\0"
    for index, name in enumerate(delay):
        struct.pack_into("<II", blob, delay_at + 32 * index, 1, section_rva + len(blob))
        blob += name.encode() + b"\0"
    header = bytearray(section_raw)
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x40)
    header[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", header, 0x44, machine, 1, 0, 0, 0, 240, 0x22)
    optional = 0x58
    struct.pack_into("<H", header, optional, 0x20B)
    struct.pack_into("<I", header, optional + 108, 16)
    struct.pack_into("<II", header, optional + 112 + 8, section_rva + imports_at, 20 * len(names))
    if delay:
        struct.pack_into("<II", header, optional + 112 + 8 * 13, section_rva + delay_at, 32)
    table = optional + 240
    header[table : table + 8] = b".idata\0\0"
    struct.pack_into("<IIII", header, table + 8, len(blob), section_rva, len(blob), section_raw)
    return bytes(header) + bytes(blob)


def write_pe(root: Path, rel: str, data: bytes) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def test_pe_imports_and_delay_imports_are_read() -> None:
    machine, names = imports_check.imports(make_pe(0x8664, ["KERNEL32.dll"], ["bcrypt.dll"]))
    assert machine == 0x8664 and names == ["bcrypt.dll", "kernel32.dll"]
    with pytest.raises(imports_check.PeError):
        imports_check.imports(b"#!/bin/sh\n" + bytes(100))


def test_a_clean_bundle_passes(tmp_path: Path) -> None:
    write_pe(tmp_path, "build/tools/vmaf.exe", make_pe(0x8664, ["KERNEL32.dll"]))
    write_pe(tmp_path, "tests/test_cpu.exe", make_pe(0x8664, ["kernel32.dll", "ADVAPI32.dll"]))
    write_pe(tmp_path, "runtime/python.exe",
             make_pe(0x8664, ["python313.dll", "VCRUNTIME140.dll", "api-ms-win-crt-heap-l1-1-0.dll"]))  # fmt: skip
    write_pe(tmp_path, "runtime/python313.dll", make_pe(0x8664, ["kernel32.dll"]))
    write_pe(tmp_path, "runtime/vcruntime140.dll", make_pe(0x8664, ["kernel32.dll"]))
    write_pe(tmp_path, "runtime/DLLs/_ctypes.pyd", make_pe(0x8664, ["libffi-8.dll"]))
    write_pe(tmp_path, "runtime/DLLs/libffi-8.dll", make_pe(0x8664, ["kernel32.dll"]))
    assert imports_check.check(tmp_path, "x64") == []
    assert imports_check.main([str(tmp_path), "--machine", "x64"]) == 0


def test_planted_defects_are_found(tmp_path: Path) -> None:
    write_pe(tmp_path, "build/tools/vmaf.exe", make_pe(0x8664, ["VCRUNTIME140.dll"]))
    write_pe(tmp_path, "tests/test_a.exe", make_pe(0x8664, ["api-ms-win-crt-runtime-l1-1-0.dll"]))
    write_pe(tmp_path, "tests/test_b.exe", make_pe(0xAA64, ["kernel32.dll"]))
    write_pe(tmp_path, "tests/test_c.exe", make_pe(0x8664, ["libvendor.dll"]))
    write_pe(tmp_path, "runtime/python.exe", make_pe(0x8664, ["python313.dll"]))
    write_pe(tmp_path, "tests/notes.dll", b"not a PE")
    problems = imports_check.check(tmp_path, "x64")
    joined = "\n".join(problems)
    assert "build/tools/vmaf.exe: imports the runtime DLL vcruntime140.dll" in joined
    assert "tests/test_a.exe: imports the runtime DLL api-ms-win-crt-runtime" in joined
    assert "tests/test_b.exe: machine 0xaa64, expected 0x8664" in joined
    assert "tests/test_c.exe: imports libvendor.dll, which is not part of Windows" in joined
    assert "runtime/python.exe: imports python313.dll, neither part of Windows" in joined
    assert "tests/notes.dll: not a readable PE image" in joined
    assert imports_check.main([str(tmp_path), "--machine", "x64"]) == 1


def test_an_empty_bundle_is_not_clean(tmp_path: Path) -> None:
    assert imports_check.check(tmp_path, "arm64") == ["no VMAFx program under build/ or tests/"]


def sycl_bundle(root: Path) -> None:
    """A /MD bundle shaped like the SYCL zip: the runtime DLLs beside each program."""
    for directory, program in (("build/tools", "vmaf.exe"), ("tests", "test_sycl_psnr.exe")):
        write_pe(root, f"{directory}/{program}",
                 make_pe(0x8664, ["sycl8.dll", "ze_loader.dll", "VCRUNTIME140.dll", "MSVCP140.dll",
                                  "api-ms-win-crt-runtime-l1-1-0.dll", "KERNEL32.dll"]))  # fmt: skip
        write_pe(root, f"{directory}/sycl8.dll",
                 make_pe(0x8664, ["ur_win_proxy_loader.dll", "vcruntime140.dll"]))  # fmt: skip
        write_pe(root, f"{directory}/ur_win_proxy_loader.dll", make_pe(0x8664, ["ucrtbase.dll"]))
        write_pe(root, f"{directory}/ur_loader.dll", make_pe(0x8664, ["msvcp140.dll"]))
        write_pe(root, f"{directory}/ze_loader.dll",
                 make_pe(0x8664, ["kernel32.dll", "cfgmgr32.dll"]))  # fmt: skip
        write_pe(root, f"{directory}/vcruntime140.dll", make_pe(0x8664, ["kernel32.dll"]))
        write_pe(root, f"{directory}/msvcp140.dll", make_pe(0x8664, ["vcruntime140.dll"]))


def test_a_md_bundle_passes_with_its_runtime_beside_each_program(tmp_path: Path) -> None:
    sycl_bundle(tmp_path)
    assert imports_check.check(tmp_path, "x64", "md", ("ur_loader.dll",)) == []
    assert imports_check.main([str(tmp_path), "--machine", "x64", "--runtime", "md",
                               "--loaded-at-run-time", "ur_loader.dll"]) == 0  # fmt: skip
    # The same tree is refused by the /MT check of the CPU and CUDA zips.
    assert any("imports the runtime DLL" in p for p in imports_check.check(tmp_path, "x64"))


def test_cfgmgr32_is_a_windows_dll_and_an_unknown_one_is_not(tmp_path: Path) -> None:
    """The Level Zero loader imports cfgmgr32.dll (System32); an unknown DLL still fails."""
    sycl_bundle(tmp_path)
    assert imports_check.check(tmp_path, "x64", "md", ("ur_loader.dll",)) == []
    write_pe(tmp_path, "tests/ze_loader.dll", make_pe(0x8664, ["kernel32.dll", "libvendor.dll"]))
    assert (
        "tests/ze_loader.dll: imports libvendor.dll, neither part of Windows nor in its directory"
        in imports_check.check(tmp_path, "x64", "md", ("ur_loader.dll",))
    )


def test_a_md_bundle_refuses_a_missing_or_unused_dll(tmp_path: Path) -> None:
    sycl_bundle(tmp_path)
    (tmp_path / "tests/msvcp140.dll").unlink()
    write_pe(tmp_path, "tests/opencl_adapter.dll", make_pe(0x8664, ["kernel32.dll"]))
    problems = imports_check.check(tmp_path, "x64", "md", ("ur_loader.dll",))
    assert (
        "tests/test_sycl_psnr.exe: imports msvcp140.dll, neither part of Windows nor in its directory"
        in problems
    )
    assert any(
        p.startswith("tests/opencl_adapter.dll: no file in its directory imports it")
        for p in problems
    )
    # ur_loader.dll is loaded by name at run time; without the allowance it is unused.
    assert any(
        p.startswith("build/tools/ur_loader.dll: no file")
        for p in imports_check.check(tmp_path, "x64", "md")
    )


# ---- build script steps --------------------------------------------------------------------


def test_the_build_needs_every_input() -> None:
    env = {name: "x" for name in builder.REQUIRED}
    env["VMAFX_ARCH"] = "x64"
    assert builder.require_environment(env)["VMAFX_ARCH"] == "x64"
    with pytest.raises(builder.BuildError, match="missing environment: PBS_URL"):
        builder.require_environment({**env, "PBS_URL": ""})
    with pytest.raises(builder.BuildError, match="VMAFX_ARCH"):
        builder.require_environment({**env, "VMAFX_ARCH": "x86"})


def touch(root: Path, *rels: str) -> None:
    for rel in rels:
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(b"x")


def test_prune_keeps_the_interpreter_and_drops_what_the_report_does_not_use(tmp_path) -> None:
    keep = ("python.exe", "python313.dll", "vcruntime140.dll", "LICENSE.txt", "DLLs/_ctypes.pyd",
            "DLLs/libffi-8.dll", "Lib/json/__init__.py", "Lib/site-packages/README.txt")  # fmt: skip
    drop = ("pythonw.exe", "include/Python.h", "libs/python313.lib", "Scripts/pip.exe",
            "tcl/tcl8.6/init.tcl", "DLLs/tcl86t.dll", "DLLs/tk86t.dll", "DLLs/_tkinter.pyd",
            "DLLs/zlib1.dll", "DLLs/_testcapi.pyd", "DLLs/_ctypes_test.pyd", "Lib/test/x.py",
            "Lib/idlelib/x.py", "Lib/tkinter/x.py", "Lib/ensurepip/x.py",
            "Lib/site-packages/pip/x.py", "Lib/site-packages/pip-25.2.dist-info/RECORD",
            "Lib/json/__pycache__/x.pyc", "Lib/stray.pyc")  # fmt: skip
    touch(tmp_path, *keep, *drop)
    builder.prune_runtime(tmp_path)
    assert all((tmp_path / rel).exists() for rel in keep)
    assert not [rel for rel in drop if (tmp_path / rel).exists()]


def test_the_runtime_dlls_come_unmodified_from_the_redist_folder(tmp_path: Path) -> None:
    crt = tmp_path / "redist" / "x64" / "Microsoft.VC145.CRT"
    crt.mkdir(parents=True)
    (crt / "vcruntime140.dll").write_bytes(b"microsoft-140")
    (crt / "vcruntime140_1.dll").write_bytes(b"microsoft-140-1")
    runtime = tmp_path / "bundle" / "runtime"
    runtime.mkdir(parents=True)
    (runtime / "vcruntime140.dll").write_bytes(b"pbs-140")
    (runtime / "vcruntime140_1.dll").write_bytes(b"pbs-140-1")
    assert builder.redist_crt_dir(tmp_path / "redist", "x64") == crt
    copied = builder.replace_vc_runtime(runtime, crt)
    assert [entry["file"] for entry in copied] == ["runtime/vcruntime140.dll",
                                                   "runtime/vcruntime140_1.dll"]  # fmt: skip
    assert (runtime / "vcruntime140.dll").read_bytes() == b"microsoft-140"
    (crt / "vcruntime140_1.dll").unlink()
    with pytest.raises(builder.BuildError, match="vcruntime140_1.dll is not in"):
        builder.replace_vc_runtime(runtime, crt)
    with pytest.raises(builder.BuildError, match="Microsoft.VC14"):
        builder.redist_crt_dir(tmp_path / "redist", "arm64")


def test_pack_is_reproducible_and_has_one_top_level_folder(tmp_path: Path) -> None:
    bundle = tmp_path / "vmafx-tester-windows-x64-v1"
    touch(bundle, "run.cmd", "runtime/python.exe", "tests/test_cpu.exe")
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    first = builder.pack(bundle, tmp_path / "a")
    second = builder.pack(bundle, tmp_path / "b")
    assert first.read_bytes() == second.read_bytes()
    names = zipfile.ZipFile(first).namelist()
    assert names == sorted(names) and all(n.startswith(f"{bundle.name}/") for n in names)
    digest, name = (tmp_path / "a" / f"{first.name}.sha256").read_text().split()
    assert name == first.name and digest == builder.sha256(first)


def compressible(size: int) -> bytes:
    """Text-like bytes on which zlib level 9 and zopfli write different streams."""
    words = [b"vmaf", b"motion", b"adm", b"vif", b"frame", b"score", b"0.917", b"\n"]
    state, out = 12345, bytearray()
    while len(out) < size:
        state = (state * 1103515245 + 12345) % 2**31
        out += words[state % len(words)] + b" "
    return bytes(out[:size])


def raw_deflate(data: bytes, level: int) -> bytes:
    compressor = zlib.compressobj(level, zlib.DEFLATED, -zlib.MAX_WBITS)
    return compressor.compress(data) + compressor.flush()


def zopfli_stream(data: bytes) -> bytes:
    """zopfli's raw Deflate at its default 15 iterations, computed here, not by the builder."""
    return zopfli.compress(data, numiterations=15)[2:-4]


def stored_stream(archive: Path, info: zipfile.ZipInfo) -> bytes:
    """The compressed bytes of one entry, read past its local file header."""
    with archive.open("rb") as raw:
        raw.seek(info.header_offset)
        header = raw.read(30)
        name_len, extra_len = struct.unpack("<HH", header[26:30])
        raw.seek(info.header_offset + 30 + name_len + extra_len)
        return raw.read(info.compress_size)


def bundle_of(root: Path, files: dict[str, bytes]) -> Path:
    bundle = root / "vmafx-tester-windows-x64-v1"
    for rel, data in files.items():
        (bundle / rel).parent.mkdir(parents=True, exist_ok=True)
        (bundle / rel).write_bytes(data)
    (root / "out").mkdir()
    return bundle


def test_pack_deflates_every_entry_at_the_strongest_level(tmp_path: Path) -> None:
    """ADR-1594: every entry is zopfli's Deflate stream (method 8). The zip was zlib
    level 6 (ADR-1591 found writestr() ignoring the ZipFile's level), then zlib level 9."""
    data = compressible(300_000)
    assert zopfli_stream(data) != raw_deflate(data, 9)  # the check can tell them apart
    assert zlib.decompress(zopfli_stream(data), -zlib.MAX_WBITS) == data
    rels = ("run.cmd", "tests/test_cpu.exe", "python/test/resource/yuv/a.yuv")
    bundle = bundle_of(tmp_path, dict.fromkeys(rels, data))
    archive = builder.pack(bundle, tmp_path / "out")
    with zipfile.ZipFile(archive) as zf:
        infos = zf.infolist()
        assert zf.testzip() is None and all(zf.read(info) == data for info in infos)
    assert len(infos) == 3
    for info in infos:
        assert info.compress_type == zipfile.ZIP_DEFLATED
        assert stored_stream(archive, info) == zopfli_stream(data), info.filename


def zipfile_reference(bundle: Path, target: Path) -> bytes:
    """What Python's zipfile writes on Windows for the same files at zlib level 9."""
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(p for p in bundle.rglob("*") if p.is_file()):
            entry = zipfile.ZipInfo(f"{bundle.name}/{path.relative_to(bundle).as_posix()}",
                                    (1980, 1, 1, 0, 0, 0))  # fmt: skip
            entry.create_system = 0
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            zf.writestr(entry, path.read_bytes(), compresslevel=9)
    return target.read_bytes()


def test_the_container_is_what_zipfile_writes(tmp_path: Path) -> None:
    """With zlib's level-9 stream in place of zopfli's, pack() writes zipfile's bytes: the
    records Explorer, tar and Expand-Archive opened before ADR-1594 are unchanged."""
    files = {"run.cmd": b"@echo off\r\n", "tests/t\u00e9st.exe": compressible(70_000),
             "licenses/empty.txt": b""}  # fmt: skip
    bundle = bundle_of(tmp_path, files)
    archive = builder.pack(bundle, tmp_path / "out", deflate=lambda data: raw_deflate(data, 9))
    assert archive.read_bytes() == zipfile_reference(bundle, tmp_path / "reference.zip")


def test_pack_refuses_what_needs_zip64(tmp_path: Path, monkeypatch) -> None:
    bundle = bundle_of(tmp_path, {"a.bin": b"x" * 64, "b.bin": b"y"})
    monkeypatch.setattr(builder, "ZIP_MAX_SIZE", 32)
    with pytest.raises(builder.BuildError, match="a.bin: 64 bytes needs zip64"):
        builder.pack(bundle, tmp_path / "out", deflate=lambda data: raw_deflate(data, 9))
    monkeypatch.setattr(builder, "ZIP_MAX_SIZE", 1 << 20)
    monkeypatch.setattr(builder, "ZIP_MAX_ENTRIES", 1)
    with pytest.raises(builder.BuildError, match="2 files need zip64"):
        builder.pack(bundle, tmp_path / "out", deflate=lambda data: raw_deflate(data, 9))


def test_download_refuses_plain_http_and_a_wrong_hash(tmp_path: Path, monkeypatch) -> None:
    with pytest.raises(builder.BuildError, match="not an https URL"):
        builder.download("http://example.invalid/x", tmp_path / "x", "0" * 64)

    class Response:
        def __init__(self) -> None:
            self.chunks = [b"payload", b""]

        def read(self, _size: int) -> bytes:
            return self.chunks.pop(0)

        def __enter__(self):
            return self

        def __exit__(self, *_args) -> None:
            return None

    monkeypatch.setattr(builder.urllib.request, "urlopen", lambda *_a, **_k: Response())
    with pytest.raises(builder.BuildError, match="is not the pinned"):
        builder.download("https://example.invalid/x", tmp_path / "x", "0" * 64)


def test_zstd_needs_the_module_or_the_program(monkeypatch) -> None:
    def no_module(name: str):
        raise ImportError(name)

    monkeypatch.setattr(builder.importlib, "import_module", no_module)
    monkeypatch.setattr(builder.shutil, "which", lambda _name: None)
    with pytest.raises(builder.BuildError, match="neither compression.zstd"):
        builder.zstd_decompress(b"")


# ---- the CUDA zip (ADR-1516) ------------------------------------------------------------


def test_the_cuda_zip_builds_the_cuda_backend_on_x64_only(monkeypatch) -> None:
    options = builder.meson_options("cuda")
    assert "-Denable_cuda=true" in options and "-Denable_cuda=false" not in options
    assert "-Db_vscrt=mt" in options and "-Denable_cuda=false" in builder.meson_options("")
    monkeypatch.setenv("VMAFX_GPU", "cuda")
    assert builder.gpu_kit({"VMAFX_ARCH": "x64"}) == "cuda"
    with pytest.raises(builder.BuildError, match="x64 only"):
        builder.gpu_kit({"VMAFX_ARCH": "arm64"})
    monkeypatch.setenv("VMAFX_GPU", "hip")
    with pytest.raises(builder.BuildError, match="empty, cuda or sycl"):
        builder.gpu_kit({"VMAFX_ARCH": "x64"})


def test_the_cuda_eula_is_copied_only_when_it_is_the_text_adr_1509_read(tmp_path) -> None:
    toolkit, bundle = tmp_path / "cuda", tmp_path / "bundle"
    toolkit.mkdir()
    (toolkit / "LICENSE").write_text("End User License Agreement\nLast updated: May 1, 2027\n")
    with pytest.raises(builder.BuildError, match="not the CUDA EULA"):
        builder.copy_cuda_eula(bundle, toolkit)
    text = "Last updated: January 26, 2026\n... libdevice.10.bc ...\n"
    (toolkit / "LICENSE").write_text(text)
    builder.copy_cuda_eula(bundle, toolkit)
    assert (bundle / "licenses/nvidia/CUDA-EULA.txt").read_text() == text


def test_the_nv_codec_headers_notices_come_from_the_headers(tmp_path: Path) -> None:
    headers = tmp_path / "nv" / "include" / "ffnvcodec"
    headers.mkdir(parents=True)
    # Split so the repository's SPDX gate does not read this fixture as an MIT text.
    notice = "/*\n * Copyright (c) 2016\n * Permission is hereby granted, " + "free of charge\n */"
    for name in builder.NV_CODEC_HEADERS:
        (headers / name).write_text(notice + "\n#pragma once\nint code;\n")
    builder.write_nv_codec_notices(tmp_path / "bundle", tmp_path / "nv")
    text = (tmp_path / "bundle/licenses/nv-codec-headers/NOTICE.txt").read_text()
    assert text.count("Permission is hereby granted") == 2 and "int code" not in text
    (headers / "dynlink_cuda.h").write_text("#pragma once\n")
    with pytest.raises(builder.BuildError, match="no leading comment"):
        builder.write_nv_codec_notices(tmp_path / "bundle", tmp_path / "nv")


def test_an_unimported_runtime_dll_is_dropped_before_the_replacement(tmp_path: Path) -> None:
    runtime = tmp_path / "runtime"
    write_pe(runtime, "python.exe", make_pe(0xAA64, ["python313.dll", "VCRUNTIME140.dll"]))
    write_pe(runtime, "vcruntime140.dll", make_pe(0xAA64, ["kernel32.dll"]))
    # The Arm64 archive's vcruntime140_1.dll is an x64 image nothing imports.
    write_pe(runtime, "vcruntime140_1.dll", make_pe(0x8664, ["kernel32.dll"]))
    assert builder.drop_unimported_runtime(runtime) == ["vcruntime140_1.dll"]
    assert (runtime / "vcruntime140.dll").is_file()
    assert not (runtime / "vcruntime140_1.dll").exists()
    write_pe(runtime, "vcruntime140_1.dll", make_pe(0x8664, ["kernel32.dll"]))
    write_pe(runtime, "DLLs/_wmi.pyd", make_pe(0x8664, ["vcruntime140_1.dll"]))
    assert builder.drop_unimported_runtime(runtime) == []  # x64: _wmi.pyd needs it


def test_the_log_shows_the_output_of_a_failed_unit_test(tmp_path: Path, capsys) -> None:
    bundle = tmp_path / "bundle"
    script = bundle / "tests" / "test_b.sh"
    script.parent.mkdir(parents=True)
    script.write_text("#!/bin/sh\necho 'FAIL: test_b: a wavelet kernel differs'\nexit 1\n")
    script.chmod(0o755)
    (bundle / "image").mkdir()
    manifest = {"tests": [{"name": "test_a", "cmd": "tests/test_a.sh"},
                          {"name": "test_b", "cmd": "tests/test_b.sh"}]}  # fmt: skip
    (bundle / "image" / "unit-tests.json").write_text(json.dumps(manifest))
    report = tmp_path / "report.json"
    report.write_text(json.dumps({"unit_tests": {"failures": ["test_b"]}}))
    assert [n for n, _ in builder.failed_test_commands(bundle, report)] == ["test_b"]
    builder.print_failed_tests(bundle, report)
    out = capsys.readouterr().out
    assert "a wavelet kernel differs" in out and "test_b exit status: 1" in out
    assert "test_a" not in out


# ---- the SYCL zip (ADR-1566) ------------------------------------------------------------


def test_the_sycl_zip_builds_with_icx_cl_and_the_dynamic_runtime_on_x64_only(monkeypatch) -> None:
    options = builder.meson_options("sycl")
    assert "-Denable_sycl=true" in options and "-Db_vscrt=md" in options
    assert "-Db_vscrt=mt" not in options and "-Denable_sycl=false" not in options
    assert builder.build_environment("sycl")["CC"] == "icx-cl"
    assert (
        "CC" not in builder.build_environment("") or builder.build_environment("")["CC"] != "icx-cl"
    )
    assert builder.KITS["sycl"] == ("windows-sycl-zip", "-sycl")
    monkeypatch.setenv("VMAFX_GPU", "sycl")
    assert builder.gpu_kit({"VMAFX_ARCH": "x64"}) == "sycl"
    with pytest.raises(builder.BuildError, match="SYCL zip is built for x64 only"):
        builder.gpu_kit({"VMAFX_ARCH": "arm64"})
    assert builder.import_check_arguments("windows-sycl-zip")[:2] == ["--runtime", "md"]
    assert builder.import_check_arguments("windows-cuda-zip") == ["--runtime", "mt"]


def test_the_scratch_audit_reads_the_ratchet_list_the_zip_carries(
    tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.chdir(_REPO)
    image = tmp_path / "image"
    image.mkdir()
    tests = [{"name": "test_sycl_psnr_parity", "cmd": "tests/test_sycl_psnr_parity.exe"},
             {"name": "test_sycl_kernel_scratch", "cmd": "tests/test_sycl_kernel_scratch.exe"}]  # fmt: skip
    (image / "gpu-tests.json").write_text(json.dumps({"tests": tests, "left_out": []}))
    builder.point_scratch_audit_at_bundle(tmp_path)
    entries = {t["name"]: t for t in json.loads((image / "gpu-tests.json").read_text())["tests"]}
    assert entries["test_sycl_kernel_scratch"]["env"] == {
        "VMAF_SYCL_SCRATCH_RATCHET_FILE": "{root}/image/scratch_ratchet.txt"
    }
    assert "env" not in entries["test_sycl_psnr_parity"]
    assert (image / "scratch_ratchet.txt").read_bytes() == (
        _REPO / builder.SCRATCH_RATCHET
    ).read_bytes()
    (image / "gpu-tests.json").write_text(json.dumps({"tests": tests[:1], "left_out": []}))
    with pytest.raises(builder.BuildError, match="0 times, not once"):
        builder.point_scratch_audit_at_bundle(tmp_path)


def test_the_level_zero_loader_lies_beside_every_program(tmp_path: Path) -> None:
    prefix, bundle = tmp_path / "lz", tmp_path / "bundle"
    for directory in builder.PROGRAM_DIRS:
        (bundle / directory).mkdir(parents=True)
    with pytest.raises(builder.BuildError, match="build the Level Zero loader first"):
        builder.stage_level_zero_loader(bundle, prefix)
    (prefix / "bin").mkdir(parents=True)
    (prefix / "bin/ze_loader.dll").write_bytes(make_pe(0x8664, ["kernel32.dll"]))
    builder.stage_level_zero_loader(bundle, prefix)
    assert all((bundle / d / "ze_loader.dll").is_file() for d in builder.PROGRAM_DIRS)


def test_the_programs_runtime_closure_comes_from_the_redistributable_folder(tmp_path: Path) -> None:
    crt, bundle = tmp_path / "crt", tmp_path / "bundle"
    crt.mkdir()
    write_pe(crt, "vcruntime140.dll", make_pe(0x8664, ["kernel32.dll"]))
    write_pe(crt, "vcruntime140_1.dll", make_pe(0x8664, ["vcruntime140.dll"]))
    write_pe(crt, "msvcp140.dll", make_pe(0x8664, ["vcruntime140.dll", "vcruntime140_1.dll"]))
    write_pe(crt, "concrt140.dll", make_pe(0x8664, ["kernel32.dll"]))
    for directory in builder.PROGRAM_DIRS:
        write_pe(bundle, f"{directory}/prog.exe", make_pe(0x8664, ["sycl8.dll", "kernel32.dll"]))
        write_pe(bundle, f"{directory}/sycl8.dll", make_pe(0x8664, ["MSVCP140.dll"]))
    copied = builder.copy_program_runtime(bundle, crt)
    names = sorted({entry["file"] for entry in copied})
    assert names == [f"{d}/{n}" for d in builder.PROGRAM_DIRS
                     for n in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")]  # fmt: skip
    assert not (bundle / "tests/concrt140.dll").exists()  # nothing imports it
    write_pe(bundle, "tests/extra.dll", make_pe(0x8664, ["vccorlib140.dll"]))
    with pytest.raises(builder.BuildError, match="vccorlib140.dll is imported beside the programs"):
        builder.copy_program_runtime(bundle, crt)


def test_unimported_intel_dlls_are_dropped_until_nothing_changes(tmp_path: Path) -> None:
    for directory in builder.PROGRAM_DIRS:
        write_pe(tmp_path, f"{directory}/prog.exe", make_pe(0x8664, ["sycl8.dll", "libmmd.dll"]))
        write_pe(tmp_path, f"{directory}/sycl8.dll", make_pe(0x8664, ["ur_win_proxy_loader.dll"]))
        write_pe(
            tmp_path, f"{directory}/ur_win_proxy_loader.dll", make_pe(0x8664, ["kernel32.dll"])
        )
        write_pe(tmp_path, f"{directory}/libmmd.dll", make_pe(0x8664, ["kernel32.dll"]))
        write_pe(tmp_path, f"{directory}/ur_loader.dll", make_pe(0x8664, ["umf.dll"]))
        write_pe(tmp_path, f"{directory}/umf.dll", make_pe(0x8664, ["libhwloc-15.dll"]))
        write_pe(tmp_path, f"{directory}/libhwloc-15.dll", make_pe(0x8664, ["kernel32.dll"]))
        # libirngmd imports libircmd: dropping the first orphans the second.
        write_pe(tmp_path, f"{directory}/libirngmd.dll", make_pe(0x8664, ["libircmd.dll"]))
        write_pe(tmp_path, f"{directory}/libircmd.dll", make_pe(0x8664, ["kernel32.dll"]))
    names = {"sycl8.dll", "ur_win_proxy_loader.dll", "libmmd.dll", "ur_loader.dll", "umf.dll",
             "libhwloc-15.dll", "libirngmd.dll", "libircmd.dll"}  # fmt: skip
    assert builder.drop_unimported_vendor_dlls(tmp_path, names) == ["libirngmd.dll", "libircmd.dll"]
    left = sorted(p.name for p in (tmp_path / "tests").iterdir())
    assert left == ["libhwloc-15.dll", "libmmd.dll", "prog.exe", "sycl8.dll", "umf.dll",
                    "ur_loader.dll", "ur_win_proxy_loader.dll"]  # fmt: skip
    assert builder.drop_unimported_vendor_dlls(tmp_path, names) == []


def test_the_shipped_windows_sycl_runtime_list_names_redistributable_dlls() -> None:
    spec = json.loads((_REPO / builder.SYCL_RUNTIME).read_text())
    assert spec["credist_dir"] == "bin"
    names = {n for c in spec["components"] for n in c["names"]}
    assert all(n.endswith(".dll") and "*" not in n for n in names)
    assert {"sycl8.dll", "ur_loader.dll", "umf.dll", "libhwloc-15.dll"} <= names
    # Debug builds, OpenCL and the JIT are not what the AOT programs load.
    assert not {"sycl8d.dll", "ur_adapter_opencl.dll", "sycl-jit.dll", "OpenCL.dll"} & names
    assert set(builder.LOADED_AT_RUN_TIME) <= names
    assert all(c["dests"] == list(builder.PROGRAM_DIRS) for c in spec["components"])
