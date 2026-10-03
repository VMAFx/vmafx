# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the build-time helper of the tester image (image/prepare_build.py)."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

_PATH = Path(__file__).resolve().parents[1] / "image" / "prepare_build.py"
_spec = importlib.util.spec_from_file_location("prepare_build", _PATH)
pb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pb)


def test_listed_names_skip_comments_and_blanks(tmp_path: Path) -> None:
    path = tmp_path / "list.txt"
    path.write_text("# comment\n\ntest_a\n  test_b  \n")
    assert pb.listed_names(path) == ["test_a", "test_b"]


def fake_introspect(monkeypatch, build: Path, names: list[str], python: tuple[str, ...] = ()):
    tests = [{"name": n, "cmd": [str(build / "test" / n)]} for n in names]
    tests += [{"name": n, "cmd": ["/usr/bin/python3", "x.py"]} for n in python]

    def run(*_args, **_kwargs):
        return subprocess.CompletedProcess([], 0, stdout=json.dumps(tests), stderr="")

    monkeypatch.setattr(pb.subprocess, "run", run)


def test_native_tests_keep_listed_executables_only(tmp_path: Path, monkeypatch) -> None:
    names = [f"test_{i}" for i in range(pb.MIN_TESTS)]
    fake_introspect(monkeypatch, tmp_path, names + ["test_unlisted"], python=["test_py"])
    found = pb.native_tests(tmp_path, [*names, "test_py", "test_absent"])
    assert [item["name"] for item in found] == sorted(names)  # no python test, no absent one


def test_too_few_tests_fail_the_build(tmp_path: Path, monkeypatch) -> None:
    fake_introspect(monkeypatch, tmp_path, ["test_only_one"])
    with pytest.raises(pb.BuildError):
        pb.native_tests(tmp_path, ["test_only_one"])


def test_exactly_the_minimum_is_accepted(tmp_path: Path, monkeypatch) -> None:
    names = [f"t{i}" for i in range(pb.MIN_TESTS)]
    fake_introspect(monkeypatch, tmp_path, names)
    assert len(pb.native_tests(tmp_path, names)) == pb.MIN_TESTS


def test_report_fixture_lines(tmp_path: Path) -> None:
    manifest = tmp_path / "m.sha256"
    manifest.write_text("a1  yuv/one.yuv\nb2  yuv/two.yuv\nc3  yuv/unused.yuv\n")
    fixtures = tmp_path / "f.json"
    entry = {"ref": "python/test/resource/yuv/one.yuv", "dis": "python/test/resource/yuv/two.yuv"}
    fixtures.write_text(json.dumps({"fixtures": [entry]}))
    assert pb.report_fixture_lines(manifest, fixtures) == ["a1  yuv/one.yuv", "b2  yuv/two.yuv"]
    entry["dis"] = "python/test/resource/yuv/missing.yuv"
    fixtures.write_text(json.dumps({"fixtures": [entry]}))
    with pytest.raises(pb.BuildError):
        pb.report_fixture_lines(manifest, fixtures)


def test_shipped_lists_and_manifests_agree() -> None:
    image = _PATH.parent
    names = pb.listed_names(image / "unit-tests.txt")
    macos = pb.listed_names(image / "unit-tests-macos.txt")
    assert set(names) <= set(macos) and len(names) == len(set(names))
    shipped = pb.report_fixture_lines(image / "fixtures.sha256", image / "fixtures.json")
    assert len(shipped) == 7


def test_main_reports_a_build_error_with_status_1(tmp_path: Path, capsys) -> None:
    manifest = tmp_path / "m.sha256"
    manifest.write_text("a1  yuv/one.yuv\n")
    fixtures = tmp_path / "f.json"
    entry = {"ref": "python/test/resource/yuv/x.yuv", "dis": "python/test/resource/yuv/x.yuv"}
    fixtures.write_text(json.dumps({"fixtures": [entry]}))
    assert pb.main(["prepare_build.py", "fixtures", str(manifest), str(fixtures)]) == 1
    assert "missing from the SHA-256 manifest" in capsys.readouterr().err
    assert pb.main(["prepare_build.py", "bogus"]) == 64


def test_staged_gate_runs_under_an_isolated_interpreter(tmp_path: Path) -> None:
    # ADR-1496: the macOS bundle carries the parity gate; it must import and parse
    # its arguments from the staged copy alone, with the bundle's -I -B flags.
    repo = Path(__file__).resolve().parents[3]
    pb.stage_gate(repo, tmp_path)
    gate = tmp_path / "tester" / "gate" / "scripts" / "ci" / "cross_backend_parity_gate.py"
    result = subprocess.run(
        [sys.executable, "-I", "-B", str(gate), "--help"],
        capture_output=True, text=True, check=False, timeout=60, cwd=tmp_path,
    )  # fmt: skip
    assert result.returncode == 0, result.stderr
    assert "--hold-exact" in result.stdout and "metal" in result.stdout
    fragments = list((tmp_path / "tester" / "gate" / "scripts" / "ci" / "exact_twins.d").iterdir())
    assert fragments
    assert list((tmp_path / "tester" / "gate" / "docs" / "adr").glob("*.md"))


def test_stage_gate_refuses_a_fragment_citing_a_missing_adr(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    for name in pb.GATE_FILES:
        (repo / name).parent.mkdir(parents=True, exist_ok=True)
        (repo / name).write_text("")
    (repo / pb.EXACT_TWINS_DIR).mkdir(parents=True)
    # An ADR id with no file (spelled in two parts: the repository's citation
    # registry reads every literal ADR id in the sources).
    missing = "ADR-" + "9999"
    (repo / pb.EXACT_TWINS_DIR / "adm.cuda").write_text(f"adr: {missing}\nevidence: x\n")
    (repo / "docs" / "adr").mkdir(parents=True)
    with pytest.raises(pb.BuildError):
        pb.stage_gate(repo, tmp_path / "out")


# ---- the Intel GPU image (suites, scripts, environments, twins, runtime) ------------------


def fake_suite_introspect(monkeypatch, build: Path, source: Path) -> None:
    tests = [
        {
            "name": f"test_sycl_{i}",
            "cmd": [str(build / "test" / f"test_sycl_{i}")],
            "suite": ["libvmaf:fast", "libvmaf:gpu"],
            "timeout": 30,
        }
        for i in range(pb.MIN_TESTS)
    ]
    tests.append(
        {
            "name": "test_sycl_sg32",
            "cmd": [str(build / "test" / "test_sycl_0")],
            "suite": ["libvmaf:gpu"],
            "env": {"VMAF_SYCL_VIF_SUBGROUP_SIZE": "32"},
        }
    )
    tests.append(
        {
            "name": "test_vmaf_sycl_threads",
            "suite": ["libvmaf:gpu"],
            "timeout": 300,
            "cmd": [str(source / "core" / "tools" / "test" / "threads.sh"), "sycl"],
            "env": {
                "MESON_SOURCE_ROOT": str(source / "core"),
                "MESON_BUILD_ROOT": str(build),
                "LD_LIBRARY_PATH": str(build / "src"),
            },
            "workdir": str(build),
        }
    )
    tests.append(
        {"name": "test_cuda_gate", "suite": ["libvmaf:gpu"], "cmd": ["/usr/bin/python3", "gate.py"]}
    )
    tests.append({"name": "test_cpu_only", "suite": ["libvmaf:fast"],
                  "cmd": [str(build / "test" / "test_cpu_only")]})  # fmt: skip

    def run(*_args, **_kwargs):
        return subprocess.CompletedProcess([], 0, stdout=json.dumps(tests), stderr="")

    monkeypatch.setattr(pb.subprocess, "run", run)


def test_suite_selection_keeps_scripts_and_lists_python_tests_as_left_out(
    tmp_path: Path, monkeypatch
) -> None:
    build = tmp_path / "build"
    fake_suite_introspect(monkeypatch, build, tmp_path)
    found, left_out = pb.select_tests(build, ["suite:gpu"])
    names = [t["name"] for t in found]
    assert "test_cpu_only" not in names and "test_vmaf_sycl_threads" in names
    assert left_out == [{"name": "test_cuda_gate",
                         "reason": "Python test of the source tree; device-free, runs in CI"}]  # fmt: skip
    script = next(t for t in found if t["name"] == "test_vmaf_sycl_threads")
    assert script["kind"] == "script" and script["args"] == ["sycl"] and script["scratch"]
    assert script["env"] == {"LD_LIBRARY_PATH": "{root}/build/src", "MESON_BUILD_ROOT": "{work}",
                             "MESON_SOURCE_ROOT": "{root}"}  # fmt: skip
    entry = pb.manifest_entry(script, Path("/opt/vmafx/tests/threads.sh"))
    assert entry["timeout"] == 300 and entry["scratch"] and entry["args"] == ["sycl"]
    plain = pb.manifest_entry(found[0], Path("/opt/vmafx/tests/test_sycl_0"))
    assert set(plain) == {"name", "cmd"}  # Meson's default timeout and no extras: CPU form


def test_stage_writes_shared_executables_once_and_left_out(tmp_path: Path, monkeypatch) -> None:
    build, source, image = tmp_path / "build", tmp_path, tmp_path / "image-root"
    fake_suite_introspect(monkeypatch, build, source)
    (build / "test").mkdir(parents=True)
    for i in range(pb.MIN_TESTS):
        (build / "test" / f"test_sycl_{i}").write_text("elf")
    (source / "core" / "tools" / "test").mkdir(parents=True)
    (source / "core" / "tools" / "test" / "threads.sh").write_text("#!/bin/sh\n")
    pb.stage(build, ["suite:gpu"], image, "gpu-tests.json")
    document = json.loads((image / "image" / "gpu-tests.json").read_text())
    cmds = {t["name"]: t["cmd"] for t in document["tests"]}
    assert cmds["test_sycl_sg32"] == cmds["test_sycl_0"]  # one copy, two tests
    assert document["left_out"][0]["name"] == "test_cuda_gate"
    assert (image / "tests" / "threads.sh").is_file()


def test_twin_bounds_come_from_the_staged_gate(tmp_path: Path) -> None:
    repo = Path(__file__).resolve().parents[3]
    pb.stage_gate(repo, tmp_path)
    bounds = pb.twin_bounds(tmp_path, "sycl")["features"]
    assert bounds["psnr"]["bound"] == "0" and bounds["psnr"]["extractor"] == "psnr_sycl"
    assert bounds["ssim"]["extractor"] == "integer_ssim_sycl"
    assert float(bounds["ciede"]["bound"]) == 1e-9 and bounds["ciede"]["source"].startswith("libm")
    assert bounds["float_ssim_lcs"]["options"] == "enable_lcs=true"


def fake_oneapi(root: Path, listed: list[str]) -> Path:
    lib = root / "compiler" / "latest" / "lib"
    lib.mkdir(parents=True)
    (lib / "libsycl.so.9.0.0").write_bytes(b"\x7fELF sycl")
    (lib / "libsycl.so.9").symlink_to("libsycl.so.9.0.0")
    docs = root / "compiler" / "latest" / "share" / "doc"
    docs.mkdir(parents=True)
    (docs / "credist.txt").write_text("".join(f"<installdir>/lib/{n}\n" for n in listed))
    (docs / "LICENSE").write_text("EULA")
    return root


SPEC = {"credist": "compiler/latest/share/doc/credist.txt", "components": [
    {"id": "dpcpp", "dir": "compiler/latest/lib", "credist": True,
     "names": ["libsycl.so.9", "libsycl.so.9.0.0"],
     "licences": ["compiler/latest/share/doc/LICENSE"]}]}  # fmt: skip


def test_intel_runtime_copies_listed_files_unmodified(tmp_path: Path) -> None:
    oneapi = fake_oneapi(tmp_path / "oneapi", ["libsycl.so.9", "libsycl.so.9.0.0"])
    (tmp_path / "spec.json").write_text(json.dumps(SPEC))
    pb.stage_vendor_runtime(tmp_path / "spec.json", oneapi, tmp_path / "img")
    lib = tmp_path / "img" / "lib" / "intel"
    assert (lib / "libsycl.so.9").is_symlink() and (
        lib / "libsycl.so.9.0.0"
    ).read_bytes() == b"\x7fELF sycl"
    assert (tmp_path / "img" / "licenses" / "intel" / "dpcpp" / "LICENSE").read_text() == "EULA"


def test_intel_runtime_refuses_files_credist_does_not_list(tmp_path: Path) -> None:
    oneapi = fake_oneapi(tmp_path / "oneapi", ["libsycl.so.9.0.0"])  # the link is not listed
    (tmp_path / "spec.json").write_text(json.dumps(SPEC))
    with pytest.raises(pb.BuildError, match="not in credist.txt"):
        pb.stage_vendor_runtime(tmp_path / "spec.json", oneapi, tmp_path / "img")
    missing = {**SPEC, "components": [{**SPEC["components"][0], "names": ["libabsent.so"]}]}
    (tmp_path / "spec.json").write_text(json.dumps(missing))
    with pytest.raises(pb.BuildError, match="nothing matches"):
        pb.stage_vendor_runtime(tmp_path / "spec.json", oneapi, tmp_path / "img2")


def test_shipped_sycl_runtime_list_names_only_libraries() -> None:
    spec = json.loads((_PATH.parent / "sycl-runtime.json").read_text())
    names = [n for c in spec["components"] for n in c["names"]]
    assert all(".so" in n and "*" not in n and "gdb" not in n for n in names)
    assert "libsycl-jit.so" not in names and "libur_adapter_opencl.so.0" not in names
