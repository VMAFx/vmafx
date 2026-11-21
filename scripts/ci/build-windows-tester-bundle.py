#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Build the Windows tester zip (ADR-1515) on a hosted Windows runner.

    build-windows-tester-bundle.py <output-dir>

Runs on the hosted Windows runner of .github/workflows/windows-tester-bundle.yml,
inside an MSVC developer environment (vcvarsall, which sets VCToolsRedistDir) with
meson, ninja, xxd and, on x64, nasm on PATH. Never on a workstation.

Environment (all required):
  VMAFX_ARCH              x64 or arm64 (the zip's architecture; checked against the host)
  PBS_URL, PBS_SHA256     python-build-standalone install_only_stripped archive and SHA-256
  PBS_FULL_URL, PBS_FULL_SHA256
                          the same release's `full` archive, read only for its licence
                          texts (python/licenses, PYTHON.json; ADR-1503)
  VMAF_RESOURCE_COMMIT    Netflix/vmaf_resource commit the fixtures come from
  VMAFX_SOURCE_COMMIT, VMAFX_SOURCE_REF, VMAFX_RECIPE_COMMIT, VMAFX_IMAGE_TAG
Optional, for the CUDA zip (ADR-1516; x64 only):
  VMAFX_GPU=cuda          build the CUDA backend and its device tests
  CUDA_PATH               the toolkit scripts/ci/install-cuda-toolkit.ps1 installed (its
                          LICENSE is the CUDA EULA the notices carry)
  VMAFX_NV_CODEC_HEADERS  the nv-codec-headers checkout the build includes (its two
                          headers' notices travel with the zip)
Optional, for the SYCL zip (ADR-1566; x64 only; run inside oneAPI's setvars):
  VMAFX_GPU=sycl          build the SYCL backend with icx-cl and its device tests; -fsycl
                          requires the dynamic runtime (/MD), so the VC++ runtime DLLs,
                          Intel's SYCL runtime and the Level Zero loader lie beside every
                          program
  ONEAPI_ROOT             the oneAPI installation (setvars sets it); the runtime files of
                          tools/rc1-tester/image/sycl-runtime-windows.json come from it
  VMAFX_ONEAPI_VERSION    the installed oneAPI release, recorded in image/gpu-runtime.json
  VMAFX_SYCL_AOT_TARGETS  comma-separated targets accepted by the installed ocloc (ADR-3568)
  VMAFX_LEVEL_ZERO_PREFIX the install prefix of the Level Zero loader built from
                          LEVEL_ZERO_VERSION (bin/ze_loader.dll ships)
  LEVEL_ZERO_VERSION      that loader's release, recorded in image/gpu-runtime.json

Result in <output-dir>: vmafx-tester-windows-<arch>[-cuda|-sycl]-<tag>.zip, its .sha256,
report.json (the zip's own report run on this runner) and bundle-files.txt. The zip
carries licenses/ (THIRD_PARTY_NOTICES.txt and every licence text) and is not packed
when a file has no recorded licence (ADR-1503). The CPU and CUDA zips link the C and
C++ runtime statically (/MT); their only Microsoft DLLs are the interpreter's
vcruntime140*.dll, replaced by the unmodified copies of this runner's Visual Studio
redistributable folder. The SYCL zip copies the runtime DLLs its programs import from
that same folder.
"""

from __future__ import annotations

import filecmp
import hashlib
import importlib
import importlib.util
import io
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any, Callable, NamedTuple

REQUIRED = ("VMAFX_ARCH", "PBS_URL", "PBS_SHA256", "PBS_FULL_URL", "PBS_FULL_SHA256",
            "VMAF_RESOURCE_COMMIT", "VMAFX_SOURCE_COMMIT", "VMAFX_SOURCE_REF",
            "VMAFX_RECIPE_COMMIT", "VMAFX_IMAGE_TAG")  # fmt: skip
ARCHES = {"x64": "AMD64", "arm64": "ARM64"}  # VMAFX_ARCH -> PROCESSOR_ARCHITECTURE
IMAGE_DIR = Path("tools/rc1-tester/image")
UNIT_TESTS = IMAGE_DIR / "unit-tests-windows.txt"
CUDA_TESTS = IMAGE_DIR / "cuda-tests.txt"
SYCL_TESTS = IMAGE_DIR / "sycl-tests.txt"
SYCL_RUNTIME = IMAGE_DIR / "sycl-runtime-windows.json"
# The SYCL zip's programs and the DLLs they load lie in these directories (Windows
# searches a program's own directory before System32, ADR-1566).
PROGRAM_DIRS = ("build/tools", "tests")
# DLLs loaded at run time: only the two Level Zero adapters, because the
# 2026.2 UR loader is static inside sycl9.dll (intel/llvm v7.2.0,
# unified-runtime/CMakeLists.txt:56 UR_STATIC_LOADER default ON;
# sycl/cmake/modules/BuildUnifiedRuntime.cmake:98) so it loads them directly.
LOADED_AT_RUN_TIME = (
    "ur_adapter_level_zero.dll",
    "ur_adapter_level_zero_v2.dll",
)
UR_ASCII_DLL = re.compile(rb"(?<![a-z0-9_.-])ur_[a-z0-9_.-]+\.dll(?![a-z0-9_.-])")
UR_UTF16LE_DLL = re.compile(
    rb"(?<![a-z0-9_.-]\x00)u\x00r\x00_\x00(?:[a-z0-9_.-]\x00)+\.\x00d\x00l\x00l\x00"
    rb"(?![a-z0-9_.-]\x00)"
)
# The C and C++ runtime DLLs of the redistributable folder a /MD program imports.
MD_RUNTIME_PREFIXES = ("vcruntime140", "msvcp140", "concrt140", "vccorlib140")
SCRATCH_TEST = "test_sycl_kernel_scratch"
SCRATCH_RATCHET = Path("core/src/sycl/scratch_ratchet.txt")
# The CUDA EULA the toolkit's redistributable archives carry as LICENSE (each archive
# holds the same text); the build checks it is the text ADR-1509 read.
CUDA_EULA_MARKERS = ("Last updated: January 26, 2026", "libdevice.10.bc")
NV_CODEC_HEADERS = ("dynlink_cuda.h", "dynlink_loader.h")
RESOURCE_URL = (
    "https://raw.githubusercontent.com/Netflix/vmaf_resource/{commit}/python/test/resource/{file}"
)
MESON_OPTIONS = (
    "--buildtype=release", "--default-library=static", "-Db_vscrt=mt", "-Db_lto=false",
    "-Denable_float=true", "-Denable_tests=true", "-Denable_docs=false", "-Denable_cuda=false",
    "-Denable_sycl=false", "-Denable_hip=false", "-Denable_metal=disabled",
    "-Denable_dnn=disabled",
)  # fmt: skip
CUDA_OPTIONS = ("-Denable_cuda=true", "-Denable_nvcc=true")
# icx-cl with -fsycl links the dynamic runtime (Intel's guide: "-fsycl sets option /MD
# ... You cannot specify option /MT"); c++latest as in the Windows MSVC+SYCL CI leg.
SYCL_OPTIONS = ("-Denable_sycl=true", "-Db_vscrt=md", "-Dcpp_std=c++latest")
# Per GPU kit: (licence artifact of the zip, suffix of its name).
KITS = {"": ("windows-zip", ""), "cuda": ("windows-cuda-zip", "-cuda"),
        "sycl": ("windows-sycl-zip", "-sycl")}  # fmt: skip
# The interpreter keeps the standard library the report uses; these go (as in the macOS
# bundle: headers, import libraries, pip, tests, IDLE, Tcl/Tk and the zlib1.dll only
# Tcl links, the test extension modules, the GUI launcher).
PRUNE_DIRS = ("include", "libs", "Scripts", "tcl", "Lib/test", "Lib/idlelib", "Lib/tkinter",
              "Lib/turtledemo", "Lib/ensurepip", "Lib/lib2to3")  # fmt: skip
PRUNE_GLOBS = ("DLLs/tcl*.dll", "DLLs/tk*.dll", "DLLs/_tkinter.pyd", "DLLs/zlib1.dll",
               "DLLs/_test*.pyd", "DLLs/_ctypes_test.pyd", "pythonw.exe",
               "Lib/site-packages/pip", "Lib/site-packages/pip-*.dist-info")  # fmt: skip
VC_RUNTIME_GLOB = "vcruntime140*.dll"
PE_SUFFIXES = (".exe", ".dll", ".pyd")
FAILED_TEST_TIMEOUT = 300
FAILED_TEST_LINES = 60
DOWNLOAD_LIMIT = 512 * 1024 * 1024
USAGE_ARGS = 2
GOLDEN_NOT_APPLICABLE = (
    "the Python golden stack is not in the Windows zip; the Netflix pairs are covered by "
    "the dispatch and reference checks"
)
TIMEOUT = 600
# The zip (ADR-1591, ADR-1594): Deflate (method 8), the strongest method Windows 10's tar,
# Explorer's "Extract All" and PowerShell's Expand-Archive all read, encoded by zopfli at
# its default 15 iterations (requirements/locks/windows-tester-zip.txt). zipfile cannot
# store a stream it did not compress, so pack() writes the container itself, byte for byte
# the records zipfile writes on Windows (create system 0, version 20, no data descriptor,
# no zip64): test_windows_bundle.py compares the two.
ZIP_DOS_DATE = 1 << 5 | 1  # 1980-01-01
ZIP_DOS_TIME = 0  # 00:00:00
ZIP_VERSION = 20  # Deflate
ZIP_SYSTEM = 0  # MS-DOS, what zipfile writes on Windows
ZIP_DEFLATED = 8
ZIP_UTF8_FLAG = 0x800
ZIP_MODE = 0o644 << 16
ZIP_MAX_SIZE = int(((1 << 31) - 1) / 1.05)  # zipfile switches to zip64 above this
ZIP_MAX_ENTRIES = 0xFFFF
ZIP_LOCAL = struct.Struct("<4s2B4HL2L2H")
ZIP_CENTRAL = struct.Struct("<4s4B4HL2L5H2L")
ZIP_END = struct.Struct("<4s4H2LH")
ZOPFLI_ITERATIONS = 15


class BuildError(RuntimeError):
    """A build input or step is wrong; main() prints it and exits 1."""


def step(message: str) -> None:
    print(f"\n== {message}", flush=True)


def run(argv: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None,
        timeout: float = 3 * 3600, capture: bool = False) -> str:  # fmt: skip
    """Run a command, failing the build on a non-zero exit; its stdout when captured."""
    print("+ " + " ".join(argv), flush=True)
    # argv is assembled by this script from fixed program names and its own paths.
    result = subprocess.run(argv, cwd=cwd, env=env, timeout=timeout, check=False,  # noqa: S603
                            capture_output=capture, text=True)  # fmt: skip
    if result.returncode != 0:
        detail = (result.stderr or "")[-2000:] if capture else ""
        raise BuildError(f"{argv[0]} exited {result.returncode}\n{detail}")
    return result.stdout or ""


def require_environment(env: dict[str, str]) -> dict[str, str]:
    missing = [name for name in REQUIRED if not env.get(name)]
    if missing:
        raise BuildError("missing environment: " + ", ".join(missing))
    if env["VMAFX_ARCH"] not in ARCHES:
        raise BuildError(f"VMAFX_ARCH must be one of {sorted(ARCHES)}")
    return {name: env[name] for name in REQUIRED}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, target: Path, expected: str) -> Path:
    """Fetch a file over HTTPS, refusing it when its SHA-256 is not the expected one."""
    if not url.startswith("https://"):
        raise BuildError(f"{url} is not an https URL")
    target.parent.mkdir(parents=True, exist_ok=True)
    headers = {"User-Agent": "vmafx-tester-build"}
    request = urllib.request.Request(url, headers=headers)  # noqa: S310 (https only, above)
    response = urllib.request.urlopen(request, timeout=TIMEOUT)  # noqa: S310
    with response, target.open("wb") as out:
        for _ in range(DOWNLOAD_LIMIT // 65536 + 1):
            chunk = response.read(65536)
            if not chunk:
                break
            out.write(chunk)
        else:
            raise BuildError(f"{url} is larger than {DOWNLOAD_LIMIT} bytes")
    if sha256(target) != expected:
        raise BuildError(f"{url}: SHA-256 {sha256(target)} is not the pinned {expected}")
    return target


def prepare_build(*args: str, capture: bool = False) -> str:
    return run([sys.executable, str(IMAGE_DIR / "prepare_build.py"), *args], capture=capture)


def compiler_line(build: Path) -> str:
    """The C compiler Meson used, from its introspection (MSVC's cl has no --version)."""
    data = json.loads(run(["meson", "introspect", "--compilers", str(build)], capture=True))
    compiler = data["host"]["c"]
    return str(compiler.get("full_version") or f"{compiler['id']} {compiler['version']}")


def meson_options(gpu: str) -> list[str]:
    """The configure options: the CPU build, the CUDA build of the CUDA zip, or the SYCL
    build of the SYCL zip (dynamic runtime)."""
    options = list(MESON_OPTIONS)
    if gpu == "cuda":
        options = [o for o in options if o != "-Denable_cuda=false"] + list(CUDA_OPTIONS)
    if gpu == "sycl":
        targets = os.environ.get("VMAFX_SYCL_AOT_TARGETS")
        if not targets:
            raise BuildError("VMAFX_SYCL_AOT_TARGETS must contain the targets accepted by ocloc")
        drop = ("-Denable_sycl=false", "-Db_vscrt=mt")
        options = [o for o in options if o not in drop] + list(SYCL_OPTIONS)
        options.append(f"-Dsycl_icpx_aot_targets={targets}")
    return options


def build_environment(gpu: str) -> dict[str, str]:
    """MSVC's cl, or icx-cl for the SYCL build (as the Windows MSVC+SYCL CI leg)."""
    env = {**os.environ, "CFLAGS": "/experimental:c11atomics",
           "CXXFLAGS": "/experimental:c11atomics"}  # fmt: skip
    if gpu == "sycl":
        env.update({"CC": "icx-cl", "CXX": "icx-cl"})
    return env


def configure_and_build(build: Path, gpu: str) -> None:
    runtime = "dynamic C runtime /MD, icx-cl" if gpu == "sycl" else "static C runtime /MT, MSVC"
    step(f"configure and build (static libvmaf, {runtime}, GPU: {gpu or 'none'})")
    run(["meson", "setup", str(build), "core", *meson_options(gpu)], env=build_environment(gpu))
    lists = [UNIT_TESTS, {"cuda": CUDA_TESTS, "sycl": SYCL_TESTS}[gpu]] if gpu else [UNIT_TESTS]
    targets = set()
    for tests in lists:
        targets |= set(prepare_build("select", str(build), str(tests), capture=True).split())
    run(["ninja", "-C", str(build), "tools/vmaf.exe", *sorted(targets)], env=build_environment(gpu))


def stage(build: Path, bundle: Path, env: dict[str, str]) -> None:
    step("stage the bundle")
    (bundle / "build" / "tools").mkdir(parents=True)
    shutil.copy2(build / "tools" / "vmaf.exe", bundle / "build" / "tools" / "vmaf.exe")
    prepare_build("stage", str(build), str(UNIT_TESTS), str(bundle))
    shutil.copytree("tools/rc1-tester/src", bundle / "tester" / "src",
                    ignore=shutil.ignore_patterns("__pycache__"))  # fmt: skip
    shutil.copy2("tools/rc1-tester/vmaf-tester-report", bundle / "tester")
    shutil.copy2(IMAGE_DIR / "fixtures.json", bundle / "image" / "fixtures.json")
    (bundle / "image" / "package-arch.txt").write_text(ARCHES[env["VMAFX_ARCH"]] + "\n")
    shutil.copy2(IMAGE_DIR / "windows" / "run.cmd", bundle / "run.cmd")
    shutil.copy2(IMAGE_DIR / "windows" / "README.txt", bundle / "README.txt")


def copy_cuda_eula(bundle: Path, cuda_path: Path) -> None:
    """The CUDA EULA of the toolkit that compiled the kernels, unmodified, checked."""
    eula = cuda_path / "LICENSE"
    text = eula.read_text(encoding="utf-8", errors="replace") if eula.is_file() else ""
    missing = [marker for marker in CUDA_EULA_MARKERS if marker not in text]
    if missing:
        raise BuildError(f"{eula} is not the CUDA EULA ADR-1509 read (missing {missing})")
    target = bundle / "licenses" / "nvidia" / "CUDA-EULA.txt"
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(eula, target)


def header_notice(header: Path) -> str:
    """The leading comment of a header: its copyright and permission notice."""
    text = header.read_text(encoding="utf-8", errors="replace")
    end = text.find("*/")
    if end < 0:
        raise BuildError(f"{header} has no leading comment")
    return text[: end + 2]


def write_nv_codec_notices(bundle: Path, checkout: Path) -> None:
    """The notices of the two nv-codec-headers files libvmaf compiles its loader from."""
    parts = []
    for name in NV_CODEC_HEADERS:
        header = checkout / "include" / "ffnvcodec" / name
        parts.append(f"include/ffnvcodec/{name} of nv-codec-headers:\n{header_notice(header)}\n")
    text = "\n".join(parts)
    if "Permission is hereby granted" not in text:
        raise BuildError("the nv-codec-headers notices hold no MIT permission notice")
    target = bundle / "licenses" / "nv-codec-headers" / "NOTICE.txt"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")


def stage_cuda(build: Path, bundle: Path) -> None:
    """The CUDA zip's device tests, parity gate, twin bounds, targets, row map and
    NVIDIA notices (the Windows counterpart of docker/Dockerfile.tester's cuda-build)."""
    step("stage the CUDA device tests, the parity gate and the NVIDIA notices")
    prepare_build("stage", str(build), str(CUDA_TESTS), str(bundle), "gpu-tests.json")
    prepare_build("gate", ".", str(bundle))
    prepare_build("twins", str(bundle), "cuda")
    prepare_build("cuda-targets", str(build), str(bundle))
    shutil.copy2(IMAGE_DIR / "cuda-rows.json", bundle / "image" / "cuda-rows.json")
    copy_cuda_eula(bundle, Path(os.environ["CUDA_PATH"]))
    write_nv_codec_notices(bundle, Path(os.environ["VMAFX_NV_CODEC_HEADERS"]))


def point_scratch_audit_at_bundle(bundle: Path) -> None:
    """test_sycl_kernel_scratch reads the ratchet list from the source path its build
    baked in, which names nothing on a tester's PC: the zip carries the list and its
    manifest entry names it in VMAF_SYCL_SCRATCH_RATCHET_FILE."""
    shutil.copy2(SCRATCH_RATCHET, bundle / "image" / "scratch_ratchet.txt")
    manifest = bundle / "image" / "gpu-tests.json"
    document = json.loads(manifest.read_text(encoding="utf-8"))
    entries = [test for test in document["tests"] if test["name"] == SCRATCH_TEST]
    if len(entries) != 1:
        raise BuildError(f"{manifest} lists {SCRATCH_TEST} {len(entries)} times, not once")
    entries[0].setdefault("env", {})[
        "VMAF_SYCL_SCRATCH_RATCHET_FILE"
    ] = "{root}/image/scratch_ratchet.txt"
    manifest.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")


def stage_level_zero_loader(bundle: Path, prefix: Path) -> None:
    """The Level Zero loader built from LEVEL_ZERO_VERSION, beside every program: libvmaf
    imports it, so no program starts without it, and the report's probe opens the same
    copy (on a PC without an Intel driver it loads and finds no GPU)."""
    loader = prefix / "bin" / "ze_loader.dll"
    if not loader.is_file():
        raise BuildError(f"{loader} is missing: build the Level Zero loader first")
    for directory in PROGRAM_DIRS:
        shutil.copy2(loader, bundle / directory / loader.name)


def vendor_dll_names() -> set[str]:
    """Every DLL name the SYCL runtime spec lists (lower case)."""
    spec = json.loads(SYCL_RUNTIME.read_text(encoding="utf-8"))
    return {name.lower() for component in spec["components"] for name in component["names"]}


def check_runtime_dll_references(bundle: Path, names: set[str]) -> None:
    """Verify whole dynamic-load names against the Intel DLLs shipped with the CLI."""
    for name in LOADED_AT_RUN_TIME:
        if UR_ASCII_DLL.fullmatch(name.encode("utf-8")) is None:
            raise BuildError(f"invalid configured Intel run-time DLL name: {name!r}")
    directory = bundle / PROGRAM_DIRS[0]
    references: dict[str, list[str]] = {name: [] for name in LOADED_AT_RUN_TIME}
    missing = set(LOADED_AT_RUN_TIME)
    found: set[str] = set()
    # The staging manifest has already passed prepare_build's component and pattern bounds.
    for filename in sorted(names):
        path = directory / filename
        if not path.is_file():
            continue
        missing.discard(filename)
        data = path.read_bytes().lower()
        tokens = {token.decode("ascii") for token in UR_ASCII_DLL.findall(data)}
        tokens.update(token.decode("utf-16le") for token in UR_UTF16LE_DLL.findall(data))
        found.update(tokens)
        for name in LOADED_AT_RUN_TIME:
            # A PE export directory can name its own DLL without loading it.
            if filename != name and name in tokens:
                references[name].append(filename)

    failures = [f"{name} (not staged)" for name in sorted(missing)]
    failures.extend(
        f"{name} (no other staged Intel DLL references it)"
        for name, sources in references.items()
        if name not in missing and not sources
    )
    if failures:
        listing = ", ".join(sorted(found)[:40]) or "(none)"
        raise BuildError(
            f"Intel run-time DLL check failed: {', '.join(failures)}. UR DLL names found: {listing}"
        )
    notice = ", ".join(f"{name} <- {' + '.join(sources)}" for name, sources in references.items())
    print(f"::notice::Intel run-time DLL references: {notice}", flush=True)


def drop_unimported_vendor_dlls(bundle: Path, names: set[str]) -> list[str]:
    """Remove the listed Intel DLLs that no file beside the programs imports and that
    the SYCL runtime does not load by name (ADR-1503 rule 1: ship only what runs). One
    removal can orphan another, so it repeats until nothing changes."""
    read = pe_imports()
    dropped: list[str] = []
    for _ in range(len(names) + 1):
        imported: set[str] = set(LOADED_AT_RUN_TIME)
        present: set[str] = set()
        for directory in PROGRAM_DIRS:
            for path in sorted((bundle / directory).iterdir()):
                if path.is_file() and path.suffix.lower() in PE_SUFFIXES:
                    imported |= set(read(path.read_bytes())[1])
                    present.add(path.name.lower())
        unused = sorted((names & present) - imported)
        if not unused:
            return dropped
        for name in unused:
            for directory in PROGRAM_DIRS:
                remove(bundle / directory / name)
            dropped.append(name)
    raise BuildError("dropping unimported Intel DLLs did not settle")


def stage_sycl(build: Path, bundle: Path) -> None:
    """The SYCL zip's device tests, parity gate, twin bounds, row map, Intel runtime and
    Level Zero loader (the Windows counterpart of docker/Dockerfile.tester's sycl-build)."""
    step("stage the SYCL device tests, the parity gate, Intel's runtime and the loader")
    prepare_build("stage", str(build), str(SYCL_TESTS), str(bundle), "gpu-tests.json")
    prepare_build("gate", ".", str(bundle))
    prepare_build("twins", str(bundle), "sycl")
    shutil.copy2(IMAGE_DIR / "sycl-rows.json", bundle / "image" / "sycl-rows.json")
    point_scratch_audit_at_bundle(bundle)
    prepare_build("intel-runtime", str(SYCL_RUNTIME), os.environ["ONEAPI_ROOT"], str(bundle))
    names = vendor_dll_names()
    stage_level_zero_loader(bundle, Path(os.environ["VMAFX_LEVEL_ZERO_PREFIX"]))
    dropped = drop_unimported_vendor_dlls(bundle, names)
    check_runtime_dll_references(bundle, names.difference(dropped))
    versions = {"oneapi": os.environ["VMAFX_ONEAPI_VERSION"],
                "level_zero_loader": os.environ["LEVEL_ZERO_VERSION"],
                "intel_dlls_not_imported": ", ".join(dropped) or "none"}  # fmt: skip
    (bundle / "image" / "gpu-runtime.json").write_text(json.dumps(versions, indent=1) + "\n")


def fetch_fixtures(bundle: Path, out: Path, commit: str) -> None:
    step("fixtures (pinned commit, SHA-256 checked)")
    lines = prepare_build("fixtures", str(IMAGE_DIR / "fixtures.sha256"),
                          str(IMAGE_DIR / "fixtures.json"), capture=True)  # fmt: skip
    (out / "fixtures-needed.sha256").write_text(lines)
    resource = bundle / "python" / "test" / "resource"
    for line in lines.splitlines():
        digest, name = line.split()
        download(RESOURCE_URL.format(commit=commit, file=name), resource / name, digest)


def remove(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def prune_runtime(runtime: Path) -> None:
    """Drop what the report does not use; never edit a file that stays."""
    for rel in PRUNE_DIRS:
        remove(runtime / rel)
    for pattern in PRUNE_GLOBS:
        for path in sorted(runtime.glob(pattern)):
            remove(path)
    for path in sorted(runtime.rglob("__pycache__"), reverse=True):
        remove(path)
    for path in sorted(runtime.rglob("*.pyc")):
        remove(path)


def redist_crt_dir(redist_root: Path, arch: str) -> Path:
    """`<VCToolsRedistDir>\\<arch>\\Microsoft.VC14x.CRT`: the one folder ADR-1503 lets a
    Microsoft runtime DLL come from (never debug_nonredist)."""
    found = sorted((redist_root / arch).glob("Microsoft.VC14*.CRT"))
    if len(found) != 1:
        raise BuildError(f"{redist_root / arch} holds {len(found)} Microsoft.VC14*.CRT folders")
    return found[0]


def pe_imports() -> Any:
    """The PE import reader of scripts/ci/check-windows-bundle-imports.py (one parser)."""
    path = Path(__file__).with_name("check-windows-bundle-imports.py")
    spec = importlib.util.spec_from_file_location("check_windows_bundle_imports", path)
    if spec is None or spec.loader is None:
        raise BuildError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.imports


def drop_unimported_runtime(runtime: Path) -> list[str]:
    """Remove the interpreter's vcruntime140*.dll that no program of the interpreter
    imports (the Arm64 archive carries an x64 vcruntime140_1.dll nothing loads):
    ADR-1503 rule 1 ships only what runs. Returns the names removed."""
    read = pe_imports()
    imported: set[str] = set()
    for path in sorted(runtime.rglob("*")):
        if path.is_file() and path.suffix.lower() in PE_SUFFIXES:
            imported |= set(read(path.read_bytes())[1])
    dropped = []
    for shipped in sorted(runtime.glob(VC_RUNTIME_GLOB)):
        if shipped.name.lower() not in imported:
            shipped.unlink()
            dropped.append(shipped.name)
    return dropped


def replace_vc_runtime(runtime: Path, crt: Path) -> list[dict[str, str]]:
    """Replace the interpreter's vcruntime140*.dll with the unmodified copies of this
    runner's Visual Studio redistributable folder; returns what was copied."""
    copied = []
    for shipped in sorted(runtime.glob(VC_RUNTIME_GLOB)):
        source = crt / shipped.name
        if not source.is_file():
            raise BuildError(f"{shipped.name} is not in {crt}")
        shutil.copyfile(source, shipped)
        if not filecmp.cmp(source, shipped, shallow=False):
            raise BuildError(f"{shipped.name} differs from {source} after the copy")
        copied.append({"file": shipped.relative_to(runtime.parent).as_posix(),
                       "source": str(source), "sha256": sha256(shipped)})  # fmt: skip
    if not copied:
        raise BuildError(f"the interpreter in {runtime} ships no {VC_RUNTIME_GLOB}")
    return copied


def program_runtime_imports(bundle: Path) -> set[str]:
    """The VC++ runtime DLLs the files beside the SYCL zip's programs import."""
    read = pe_imports()
    names: set[str] = set()
    for directory in PROGRAM_DIRS:
        for path in sorted((bundle / directory).iterdir()):
            if path.is_file() and path.suffix.lower() in PE_SUFFIXES:
                names |= {
                    n for n in read(path.read_bytes())[1] if n.startswith(MD_RUNTIME_PREFIXES)
                }
    return names


def copy_program_runtime(bundle: Path, crt: Path) -> list[dict[str, str]]:
    """Copy the runtime DLLs the SYCL zip's programs and Intel's DLLs import (and those
    they import in turn) from the redistributable folder beside every program."""
    copied: list[dict[str, str]] = []
    done: set[str] = set()
    for _ in range(len(MD_RUNTIME_PREFIXES) * 8):  # the closure is a few files deep
        wanted = sorted(program_runtime_imports(bundle) - done)
        if not wanted:
            return copied
        for name in wanted:
            source = crt / name
            if not source.is_file():
                raise BuildError(f"{name} is imported beside the programs but is not in {crt}")
            for directory in PROGRAM_DIRS:
                target = bundle / directory / name
                shutil.copyfile(source, target)
                copied.append({"file": target.relative_to(bundle).as_posix(),
                               "source": str(source), "sha256": sha256(target)})  # fmt: skip
            done.add(name)
    raise BuildError("the runtime DLL closure did not settle")


def install_interpreter(bundle: Path, out: Path, env: dict[str, str]) -> Path:
    step("interpreter (python-build-standalone, SHA-256 checked)")
    archive = download(env["PBS_URL"], out / "pbs.tar.gz", env["PBS_SHA256"])
    with tarfile.open(archive) as tar:
        tar.extractall(out / "pbs", filter="data")
    shutil.move(out / "pbs" / "python", bundle / "runtime")
    # The interpreter archive is an input, never a product of this workflow.
    remove(out / "pbs")
    remove(archive)
    runtime = bundle / "runtime"
    prune_runtime(runtime)
    dropped = drop_unimported_runtime(runtime)
    # VCToolsRedistDir of vcvarsall; Windows environment names ignore case.
    crt = redist_crt_dir(Path(os.environ["VCTOOLSREDISTDIR"]), env["VMAFX_ARCH"])
    record: dict[str, Any] = {"redist_dir": str(crt), "files": replace_vc_runtime(runtime, crt),
                              "dropped_unimported": dropped}  # fmt: skip
    if os.environ.get("VMAFX_GPU") == "sycl":
        record["program_files"] = copy_program_runtime(bundle, crt)
    (bundle / "image" / "msvc-redist.json").write_text(json.dumps(record, indent=1) + "\n")
    return runtime / "python.exe"


def zstd_decompress(data: bytes) -> bytes:
    """Python 3.14's compression.zstd, else the zstd program."""
    try:
        module = importlib.import_module("compression.zstd")
    except ImportError:
        module = None
    if module is not None:
        decompressed: bytes = module.decompress(data)
        return decompressed
    program = shutil.which("zstd")
    if program is None:
        raise BuildError("neither compression.zstd (Python 3.14) nor a zstd program exists")
    return subprocess.run([program, "-dc"], input=data, capture_output=True, check=True,  # noqa: S603
                          timeout=TIMEOUT).stdout  # fmt: skip


def pbs_licence_texts(out: Path, texts: Path, env: dict[str, str]) -> None:
    """python/licenses and PYTHON.json of the release's full archive (ADR-1503 rule 4)."""
    archive = download(env["PBS_FULL_URL"], out / "pbs-full.tar.zst", env["PBS_FULL_SHA256"])
    target = texts / "python-build-standalone"
    target.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(zstd_decompress(archive.read_bytes()))) as tar:
        for member in tar.getmembers():
            name = member.name
            if member.isfile() and (
                name.startswith("python/licenses/") or name == "python/PYTHON.json"
            ):
                handle = tar.extractfile(member)
                if handle is None:
                    raise BuildError(f"cannot read {name} from the full archive")
                (target / Path(name).name).write_bytes(handle.read())
    remove(archive)
    if not (target / "PYTHON.json").is_file():
        raise BuildError("the full archive holds no python/PYTHON.json")


def licensing(*args: str) -> None:
    run([sys.executable, "-I", "-B", str(IMAGE_DIR / "licensing.py"), *args])


def bundle_python_version(python: Path) -> str:
    code = "import platform; print(platform.python_version())"
    return run([str(python), "-I", "-c", code], capture=True).strip()


def info_environment(build: Path, env: dict[str, str], gpu: str) -> dict[str, str]:
    """What `prepare_build.py info` writes into image/build-info.json."""
    keys = ("VMAFX_SOURCE_COMMIT", "VMAFX_SOURCE_REF", "VMAFX_RECIPE_COMMIT", "VMAFX_IMAGE_TAG")
    runner = f"{os.environ.get('IMAGEOS', 'windows')}-{os.environ.get('IMAGEVERSION', 'runner')}"
    libc = "MSVC runtime and Universal CRT, linked statically (/MT)"
    if gpu == "sycl":
        libc = ("MSVC runtime DLLs beside the programs (/MD, which -fsycl requires) and the "
                "Universal CRT of Windows")  # fmt: skip
    return {
        **os.environ,
        **{key: env[key] for key in keys},
        "VMAFX_BUILT_BY_WORKFLOW": os.environ.get("VMAFX_BUILT_BY_WORKFLOW", "true"),
        "VMAFX_ARTIFACT_KIND": "windows-zip",
        "VMAFX_BASE_IMAGE": runner,
        "VMAFX_COMPILER": compiler_line(build),
        "VMAFX_LIBC": f"{libc}; built on Windows {platform.version()}",
        "VMAFX_NOT_APPLICABLE": json.dumps({"golden": GOLDEN_NOT_APPLICABLE}),
        "VMAFX_GPU_BACKEND": gpu,
    }


def records(bundle: Path, build: Path, python: Path, env: dict[str, str], gpu: str) -> None:
    step("bundle metadata and references")
    run([sys.executable, str(IMAGE_DIR / "prepare_build.py"), "info", str(bundle)],
        env=info_environment(build, env, gpu))  # fmt: skip
    run([str(python), "-I", "-B", str(bundle / "tester" / "vmaf-tester-report"),
         "--image-root", str(bundle), "generate-reference", str(bundle / "reference")])  # fmt: skip


def run_own_report(bundle: Path, out: Path) -> None:
    step("the zip's own report on this runner (run.cmd, as a tester runs it)")
    with (out / "report.stderr").open("w", encoding="utf-8") as stderr:
        shell = os.environ.get("COMSPEC", r"C:\Windows\System32\cmd.exe")
        result = subprocess.run([shell, "/d", "/c", str(bundle / "run.cmd")], cwd=out,  # noqa: S603
                                stdout=subprocess.DEVNULL, stderr=stderr,
                                timeout=4 * 3600, check=False)  # fmt: skip
    print((out / "report.stderr").read_text(encoding="utf-8", errors="replace"))
    print(f"report exit status: {result.returncode}")
    if result.returncode not in (0, 1) or not (out / "report.json").is_file():
        raise BuildError(f"the report did not complete (exit {result.returncode})")
    print_failed_tests(bundle, out / "report.json")


def failed_test_commands(bundle: Path, report: Path) -> list[tuple[str, list[str]]]:
    """(name, argv) of every unit test the report counts as failed."""
    failures = set(json.loads(report.read_text(encoding="utf-8"))["unit_tests"]["failures"])
    manifest = json.loads((bundle / "image" / "unit-tests.json").read_text(encoding="utf-8"))
    found = []
    for test in manifest["tests"]:
        if test["name"] in failures:
            argv = [str(bundle / test["cmd"]), *[str(arg) for arg in test.get("args", [])]]
            found.append((test["name"], argv))
    return found


def print_failed_tests(bundle: Path, report: Path) -> None:
    """The output of every failed unit test, in this log only: the report keeps names,
    and a maintainer needs the failing case (the zip is not changed)."""
    for name, argv in failed_test_commands(bundle, report):
        step(f"output of the failed unit test {name} (diagnostics; not part of the zip)")
        with tempfile.TemporaryDirectory(prefix="vmaf-failed-test-") as work:
            try:
                result = subprocess.run(argv, cwd=work, capture_output=True, text=True,  # noqa: S603
                                        errors="replace", timeout=FAILED_TEST_TIMEOUT, check=False)  # fmt: skip
            except (OSError, subprocess.SubprocessError) as error:
                print(f"{name} did not run: {error}")
                continue
        lines = (result.stdout + result.stderr).splitlines()
        print("\n".join(lines[-FAILED_TEST_LINES:]))
        print(f"{name} exit status: {result.returncode}")


class ZipEntry(NamedTuple):
    """One file of the zip: its name in the archive, CRC-32, size and Deflate stream."""

    name: str
    crc: int
    size: int
    stream: bytes


def zopfli_deflate(data: bytes) -> bytes:
    """The raw Deflate stream zopfli writes for data (its zlib form without the two
    header bytes and the Adler-32 trailer)."""
    try:
        zopfli = importlib.import_module("zopfli.zopfli")
    except ImportError as error:
        raise BuildError(
            "zopfli is missing: pip install --require-hashes -r "
            "requirements/locks/windows-tester-zip.txt"
        ) from error
    zlib_form: bytes = zopfli.compress(data, numiterations=ZOPFLI_ITERATIONS)
    return zlib_form[2:-4]


def zip_entry(name: str, data: bytes, deflate: Callable[[bytes], bytes]) -> ZipEntry:
    if len(data) > ZIP_MAX_SIZE:
        raise BuildError(f"{name}: {len(data)} bytes needs zip64, which pack() does not write")
    return ZipEntry(name, zlib.crc32(data), len(data), deflate(data))


def zip_name(entry: ZipEntry) -> tuple[bytes, int]:
    """The encoded name and the general-purpose flags, as zipfile writes them."""
    try:
        return entry.name.encode("ascii"), 0
    except UnicodeEncodeError:
        return entry.name.encode("utf-8"), ZIP_UTF8_FLAG


def zip_records(entry: ZipEntry, offset: int) -> tuple[bytes, bytes]:
    """The local file header (with the name) and the central directory record."""
    name, flags = zip_name(entry)
    common = (flags, ZIP_DEFLATED, ZIP_DOS_TIME, ZIP_DOS_DATE, entry.crc, len(entry.stream),
              entry.size, len(name))  # fmt: skip
    local = ZIP_LOCAL.pack(b"PK\x03\x04", ZIP_VERSION, 0, *common, 0) + name
    central = ZIP_CENTRAL.pack(b"PK\x01\x02", ZIP_VERSION, ZIP_SYSTEM, ZIP_VERSION, 0, *common,
                               0, 0, 0, 0, ZIP_MODE, offset) + name  # fmt: skip
    return local, central


def write_zip(target: Path, entries: list[ZipEntry]) -> None:
    """Local headers and streams in order, then the central directory and its end."""
    if len(entries) > ZIP_MAX_ENTRIES:
        raise BuildError(f"{len(entries)} files need zip64, which pack() does not write")
    directory = []
    with target.open("wb") as out:
        for entry in entries:
            local, central = zip_records(entry, out.tell())
            out.write(local + entry.stream)
            directory.append(central)
        start = out.tell()
        if start > ZIP_MAX_SIZE:
            raise BuildError(
                f"{target.name}: {start} bytes needs zip64, which pack() does not write"
            )
        out.write(b"".join(directory))
        size = out.tell() - start
        out.write(ZIP_END.pack(b"PK\x05\x06", 0, 0, len(entries), len(entries), size, start, 0))


def zip_entries(bundle: Path, files: list[Path],
                deflate: Callable[[bytes], bytes]) -> list[ZipEntry]:  # fmt: skip
    """Every file deflated on every processor (zopfli releases the GIL), in file order."""

    def one(path: Path) -> ZipEntry:
        return zip_entry(f"{bundle.name}/{path.relative_to(bundle).as_posix()}",
                         path.read_bytes(), deflate)  # fmt: skip

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        return list(pool.map(one, files))


def pack(bundle: Path, out: Path, deflate: Callable[[bytes], bytes] = zopfli_deflate) -> Path:
    """A zip with one top-level folder, entries sorted, every timestamp 1980-01-01,
    every entry Deflate by zopfli."""
    step("pack")
    target = out / f"{bundle.name}.zip"
    files = sorted(p for p in bundle.rglob("*") if p.is_file())
    write_zip(target, zip_entries(bundle, files, deflate))
    listing = [f"{p.stat().st_size} {p.relative_to(bundle).as_posix()}" for p in files]
    (out / "bundle-files.txt").write_text("\n".join(listing) + "\n", encoding="utf-8")
    (out / f"{target.name}.sha256").write_text(f"{sha256(target)}  {target.name}\n")
    print(
        f"{target.name}: {target.stat().st_size} bytes, {len(files)} files, sha256 {sha256(target)}"
    )
    return target


def check_host(arch: str) -> None:
    host = os.environ.get("PROCESSOR_ARCHITECTURE", "")
    if platform.system() != "Windows" or host.upper() != ARCHES[arch]:
        raise BuildError(
            f"build the {arch} zip on a {ARCHES[arch]} Windows runner, not {host or platform.system()}"
        )


def gpu_kit(env: dict[str, str]) -> str:
    """`cuda` or `sycl` for a GPU zip (x64 only), "" for the CPU zip."""
    gpu = os.environ.get("VMAFX_GPU", "")
    if gpu not in KITS:
        raise BuildError(f"VMAFX_GPU must be empty, cuda or sycl, not {gpu!r}")
    if gpu and env["VMAFX_ARCH"] != "x64":
        raise BuildError(f"the {gpu.upper()} zip is built for x64 only")
    return gpu


def assemble(bundle: Path, build: Path, out: Path, env: dict[str, str], gpu: str) -> Path:
    """Build, stage, fetch the fixtures and install the interpreter; its python.exe."""
    configure_and_build(build, gpu)
    stage(build, bundle, env)
    if gpu == "cuda":
        stage_cuda(build, bundle)
    if gpu == "sycl":
        stage_sycl(build, bundle)
    fetch_fixtures(bundle, out, env["VMAF_RESOURCE_COMMIT"])
    return install_interpreter(bundle, out, env)


def import_check_arguments(kind: str) -> list[str]:
    """The SYCL zip's programs load the runtime DLLs beside them (/MD); the others none."""
    if kind == "windows-sycl-zip":
        return ["--runtime", "md", "--loaded-at-run-time", ",".join(LOADED_AT_RUN_TIME)]
    return ["--runtime", "mt"]


def seal(bundle: Path, out: Path, kind: str, scan: Path, version: str, tag: str) -> None:
    """Notices, the import check, the zip's own report, the licence gate, the zip."""
    step("licence notices (ADR-1503)")
    licensing("notices", "--artifact", kind, "--root", str(bundle), "--repo", ".",
              "--build-scan", str(scan), "--texts", str(out / "licence-texts"),
              "--source-commit", os.environ["VMAFX_SOURCE_COMMIT"], "--tag", tag)  # fmt: skip
    step("every program loads Windows DLLs or the bundle's own")
    run([sys.executable, "scripts/ci/check-windows-bundle-imports.py", str(bundle),
         "--machine", os.environ["VMAFX_ARCH"], *import_check_arguments(kind)])  # fmt: skip
    run_own_report(bundle, out)
    step("every file of the zip has a recorded licence (ADR-1503)")
    licensing("check", "--artifact", kind, "--root", str(bundle), "--repo", ".",
              "--build-scan", str(scan), "--python-version", version,
              "--receipt", str(bundle / "licence-check.json"))  # fmt: skip
    pack(bundle, out)


def build_all(out: Path, env: dict[str, str]) -> None:
    check_host(env["VMAFX_ARCH"])
    gpu, tag = gpu_kit(env), env["VMAFX_IMAGE_TAG"]
    kind, suffix = KITS[gpu]
    name = f"{env['VMAFX_ARCH']}{suffix}"
    bundle = out / f"vmafx-tester-windows-{name}-{tag}"
    build, texts = Path("build-tester-windows").resolve(), out / "licence-texts"
    python = assemble(bundle, build, out, env, gpu)
    step("licence texts (python-build-standalone full archive, CPython Doc/license.rst)")
    pbs_licence_texts(out, texts, env)
    version = bundle_python_version(python)
    scan = out / "vmafx-sources.json"
    licensing("fetch-texts", "--artifact", kind, "--python-version", version, "--out", str(texts))
    licensing("scan-build", "--build", str(build), "--repo", ".", "--out", str(scan))
    records(bundle, build, python, env, gpu)
    seal(bundle, out, kind, scan, version, tag)


def main(argv: list[str]) -> int:
    if len(argv) != USAGE_ARGS:
        print(__doc__, file=sys.stderr)
        return 64
    out = Path(argv[1]).resolve()  # before the chdir: relative to the caller's directory
    os.chdir(Path(__file__).resolve().parents[2])
    out.mkdir(parents=True, exist_ok=True)
    try:
        build_all(out, require_environment(dict(os.environ)))
    except (BuildError, OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        print(f"build-windows-tester-bundle: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
