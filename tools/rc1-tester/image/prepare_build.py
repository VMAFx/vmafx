#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Build-time helper of docker/Dockerfile.tester (never runs on the tester's machine).

select <build_dir> <tests.txt>        print the build targets of the listed tests
stage  <build_dir> <tests.txt> <image_root> [manifest]
                                      copy the test executables, write image/<manifest>
                                      (default unit-tests.json)
info   <image_root>                   write image/build-info.json
fixtures <fixtures.sha256> <fixtures.json>
                                      print the manifest lines the report fixtures need
gate   <repo_root> <image_root>       copy the parity gate into <image_root>/tester/gate
                                      (macOS bundle, ADR-1496; SYCL image)
twins  <image_root> <backend>         write image/gpu-twins.json from the staged gate:
                                      per gate feature its metrics and the bound the
                                      gate holds the backend's twin to
intel-runtime <runtime.json> <oneapi_root> <image_root>
                                      copy the listed Intel runtime files, unmodified,
                                      to <image_root>/lib/intel and their licence texts
                                      to <image_root>/licenses/intel (Intel GPU image)
cuda-targets <build_dir> <image_root> write image/cuda-targets.json: the CUDA version
                                      and the cubin and PTX targets of the build's
                                      gencode list, from its Meson log (NVIDIA GPU image)
hip-targets <build_dir> <rocm_root> <image_root>
                                      write image/hip-targets.json: the gfx targets of
                                      the build's code objects, from its Meson log, and
                                      the ROCm release (AMD GPU image)
rocm-runtime <runtime.json> <rocm_root> <image_root>
                                      copy the listed ROCm runtime files, unmodified,
                                      keeping their directories (the libraries find
                                      each other through their RPATH), and their
                                      licence texts (AMD GPU image)

A list names one test of the Meson build per line, or `suite:<name>` for every
test of that suite. A test runs from the image when it is an executable of the
build or a shell script; a Python test reads the source tree and is listed as
left out.
"""

from __future__ import annotations

import hashlib
import importlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

MIN_TESTS = 10
# The parity gate and what it imports or reads (ADR-1496): it runs under the
# bundle's standard-library interpreter, so nothing outside these files.
GATE_FILES = (
    "scripts/__init__.py",
    "scripts/ci/cross_backend_parity_gate.py",
    "scripts/ci/cross_backend_calibration.py",
    "scripts/lib/__init__.py",
    "scripts/lib/safe_subprocess.py",
)
EXACT_TWINS_DIR = "scripts/ci/exact_twins.d"
ADR_ID = re.compile(r"ADR-(\d{4})")


class BuildError(RuntimeError):
    """The build inputs are inconsistent; main() prints it and returns 1."""


def listed_names(path: Path) -> list[str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    return [line.strip() for line in lines if line.strip() and not line.startswith("#")]


def introspect_tests(build_dir: Path) -> list[dict]:
    """`meson introspect --tests` of a build directory."""
    out = subprocess.run(
        ["meson", "introspect", "--tests", str(build_dir)],
        check=True, capture_output=True, text=True, timeout=120,
    ).stdout  # fmt: skip
    return json.loads(out)


def wanted(test: dict, names: set[str], suites: set[str]) -> bool:
    """Whether a list names the test, by its name or by one of its suites."""
    own = {str(suite).rsplit(":", 1)[-1] for suite in test.get("suite", [])}
    return test["name"] in names or bool(own & suites)


def classify(test: dict, build_dir: Path) -> tuple[str, str]:
    """(`exe`, `script` or `left_out`, the reason it is left out)."""
    command = Path(test["cmd"][0])
    if command.name.startswith("python"):
        return "left_out", "Python test of the source tree; device-free, runs in CI"
    if command.is_relative_to(build_dir):
        return "exe", ""
    if command.suffix == ".sh":
        return "script", ""
    return "left_out", f"runs {command.name}, which the image does not carry"


def portable_env(env: dict, build_dir: Path, source_root: Path) -> dict[str, str]:
    """The test's environment with build-tree paths turned into placeholders the
    report resolves: {root} the image root, {work} the test's scratch directory."""
    out = {}
    for key, value in sorted((env or {}).items()):
        if key == "MESON_SOURCE_ROOT":  # the image keeps the fixtures at {root}/python
            out[key] = "{root}"
            continue
        value = str(value).replace(str(build_dir / "src"), "{root}/build/src")
        value = value.replace(str(build_dir), "{work}").replace(str(source_root), "{root}")
        out[str(key)] = value
    return out


def describe(test: dict, kind: str, build_dir: Path, source_root: Path) -> dict:
    """One manifest entry before staging."""
    return {
        "name": test["name"],
        "kind": kind,
        "path": Path(test["cmd"][0]),
        "args": [str(arg) for arg in test["cmd"][1:]],
        "env": portable_env(test.get("env") or {}, build_dir, source_root),
        "scratch": bool(test.get("workdir")),
        "timeout": int(test.get("timeout") or 30),
    }


def select_tests(build_dir: Path, entries: list[str]) -> tuple[list[dict], list[dict]]:
    """(tests the image can run, tests left out with the reason), both by name."""
    names = {entry for entry in entries if not entry.startswith("suite:")}
    suites = {entry.removeprefix("suite:") for entry in entries if entry.startswith("suite:")}
    source_root = build_dir.parent
    found: list[dict] = []
    left_out: list[dict] = []
    for test in introspect_tests(build_dir):
        if not wanted(test, names, suites):
            continue
        kind, reason = classify(test, build_dir)
        if kind == "left_out":
            left_out.append({"name": test["name"], "reason": reason})
        else:
            found.append(describe(test, kind, build_dir, source_root))
    if len(found) < MIN_TESTS:
        raise BuildError(f"only {len(found)} of the listed tests exist in this build")
    return sorted(found, key=lambda t: t["name"]), sorted(left_out, key=lambda t: t["name"])


def native_tests(build_dir: Path, names: list[str]) -> list[dict]:
    """Meson tests from the list that are executables of this build."""
    return [test for test in select_tests(build_dir, names)[0] if test["kind"] == "exe"]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def first_line(argv: list[str]) -> str:
    result = subprocess.run(argv, capture_output=True, text=True, timeout=60, check=False)
    return (result.stdout or result.stderr).strip().splitlines()[0]


def manifest_entry(item: dict, target: Path) -> dict:
    """The manifest form of a staged test (hw_suites.run_unit_tests reads it)."""
    entry: dict = {"name": item["name"], "cmd": str(target)}
    for key in ("args", "env"):
        if item[key]:
            entry[key] = item[key]
    if item["scratch"]:
        entry["scratch"] = True
    if item["timeout"] != 30:
        entry["timeout"] = item["timeout"]
    return entry


def stage(
    build_dir: Path, names: list[str], image_root: Path, manifest: str = "unit-tests.json"
) -> None:
    tests_dir = image_root / "tests"
    tests_dir.mkdir(parents=True, exist_ok=True)
    found, left_out = select_tests(build_dir, names)
    entries = []
    for item in found:
        target = tests_dir / item["path"].name  # two tests may share one executable
        if not target.exists():
            shutil.copy2(item["path"], target)
        entries.append(manifest_entry(item, target))
    out = image_root / "image" / manifest
    out.parent.mkdir(parents=True, exist_ok=True)
    document = {"tests": entries, "left_out": left_out}
    out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")


def libc_description() -> str:
    if platform.system() == "Darwin":
        return "libSystem, macOS " + first_line(["sw_vers", "-productVersion"])
    return first_line(["ldd", "--version"])


def info(image_root: Path) -> None:
    libs = sorted((image_root / "build" / "src").glob("libvmaf.so.*.*.*"))
    data = {
        "source_commit": os.environ.get("VMAFX_SOURCE_COMMIT", "unknown"),
        "source_ref": os.environ.get("VMAFX_SOURCE_REF", "unknown"),
        "recipe_commit": os.environ.get("VMAFX_RECIPE_COMMIT", "unknown"),
        "built_by_workflow": os.environ.get("VMAFX_BUILT_BY_WORKFLOW", "false") == "true",
        "tag": os.environ.get("VMAFX_IMAGE_TAG", "unknown"),
        "base_image": os.environ.get("VMAFX_BASE_IMAGE", "unknown"),
        "image_arch": platform.machine().lower(),
        "compiler": first_line([os.environ.get("VMAFX_CC", "gcc"), "--version"]),
        "libc": libc_description(),
        "meson": first_line(["meson", "--version"]),
        "vmaf_sha256": sha256(image_root / "build" / "tools" / "vmaf"),
        "libvmaf_sha256": sha256(libs[0]) if libs else None,  # None: libvmaf is static
        "kind": os.environ.get("VMAFX_ARTIFACT_KIND", "container-image"),
        "not_applicable": json.loads(os.environ.get("VMAFX_NOT_APPLICABLE", "{}")),
        "gpu_backend": os.environ.get("VMAFX_GPU_BACKEND") or None,
    }
    out = image_root / "image" / "build-info.json"
    out.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")


def stage_gate(repo: Path, image_root: Path) -> None:
    """The gate, its fragments and the ADR files the fragments cite (the gate checks
    that each cited ADR exists when it loads them)."""
    gate = image_root / "tester" / "gate"
    for name in GATE_FILES:
        (gate / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / name, gate / name)
    fragments = sorted((repo / EXACT_TWINS_DIR).iterdir())
    if not fragments:
        raise BuildError("scripts/ci/exact_twins.d is empty")
    (gate / EXACT_TWINS_DIR).mkdir(parents=True, exist_ok=True)
    cited: set[str] = set()
    for fragment in fragments:
        shutil.copy2(fragment, gate / EXACT_TWINS_DIR / fragment.name)
        cited |= set(ADR_ID.findall(fragment.read_text(encoding="utf-8")))
    (gate / "docs" / "adr").mkdir(parents=True, exist_ok=True)
    for number in sorted(cited):
        matches = sorted((repo / "docs" / "adr").glob(f"{number}-*.md"))
        if not matches:
            raise BuildError(f"ADR-{number} cited by an exact-twin fragment has no file")
        shutil.copy2(matches[0], gate / "docs" / "adr" / matches[0].name)


def report_fixture_lines(manifest: Path, fixtures: Path) -> list[str]:
    """Lines of the SHA-256 manifest for the files fixtures.json names."""
    names = set()
    for entry in json.loads(fixtures.read_text(encoding="utf-8"))["fixtures"]:
        for key in ("ref", "dis"):
            names.add(entry[key].removeprefix("python/test/resource/"))
    lines = [line for line in manifest.read_text(encoding="utf-8").splitlines() if line.strip()]
    chosen = [line for line in lines if line.split()[1] in names]
    if len(chosen) != len(names):
        raise BuildError("a fixture of fixtures.json is missing from the SHA-256 manifest")
    return chosen


def gate_module(image_root: Path) -> Any:
    """The staged parity gate, imported from <image_root>/tester/gate."""
    gate_root = str(image_root / "tester" / "gate")
    sys.path.insert(0, gate_root)
    try:
        return importlib.import_module("scripts.ci.cross_backend_parity_gate")
    finally:
        sys.path.remove(gate_root)


def twin_bounds(image_root: Path, backend: str) -> dict:
    """Per gate feature: its metrics, the backend's extractor and options, and the
    bound the gate holds the twin to at --precision max (0 for an exact twin, the
    LIBM_TWINS bound, or the declared tolerance) with the gate's name for its source."""
    gate = gate_module(image_root)
    features = {}
    for feature in sorted(gate.FEATURE_METRICS):
        tolerance, source = gate.resolve_cell_tolerance(
            feature, fp16_features=(), calibration=None, gpu_id=None, backends=("cpu", backend)
        )
        extractor, _, options = gate.feature_extractor_name(feature, backend).partition("=")
        features[feature] = {
            "metrics": list(gate.FEATURE_METRICS[feature]),
            "extractor": extractor,
            "options": options,
            "bound": f"{float(tolerance):.17g}",
            "source": str(source),
        }
    return {"backend": backend, "features": features}


def write_twins(image_root: Path, backend: str) -> None:
    out = image_root / "image" / "gpu-twins.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    document = twin_bounds(image_root, backend)
    out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")


def credist_names(path: Path) -> set[str]:
    """File names the compiler's credist.txt lists under <installdir>/lib."""
    names = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("<installdir>/lib/"):
            names.add(line.removeprefix("<installdir>/lib/").strip())
    return names


def copy_unmodified(source: Path, target: Path) -> None:
    """A byte-for-byte copy; a symbolic link stays a link."""
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.is_symlink():
        target.symlink_to(os.readlink(source))
    else:
        shutil.copy2(source, target)


def component_files(oneapi: Path, component: dict, redistributable: set[str]) -> list[Path]:
    """The files of one component; refuses a compiler file credist.txt does not list."""
    files: list[Path] = []
    for pattern in component["names"]:
        matches = sorted((oneapi / component["dir"]).glob(pattern))
        if not matches:
            raise BuildError(f"{component['id']}: nothing matches {component['dir']}/{pattern}")
        files += matches
    if component["credist"]:
        unlisted = [path.name for path in files if path.name not in redistributable]
        if unlisted:
            raise BuildError(f"{component['id']}: not in credist.txt: {unlisted}")
    return files


def stage_vendor_runtime(spec_path: Path, vendor_root: Path, image_root: Path) -> None:
    """Copy the runtime files and licence texts a runtime spec names (sycl-runtime.json,
    hip-runtime.json), byte for byte. A component's files go to its `dest` under the
    image root (default lib/intel), its texts to <licence_dir>/<id> (default
    licenses/intel); compiler files must be in the spec's `credist` list when it has one."""
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    redistributable = credist_names(vendor_root / spec["credist"]) if spec.get("credist") else set()
    for component in spec["components"]:
        dest = image_root / component.get("dest", "lib/intel")
        for path in component_files(vendor_root, component, redistributable):
            copy_unmodified(path, dest / path.name)
        texts = image_root / spec.get("licence_dir", "licenses/intel") / component["id"]
        for text in component["licences"]:
            texts.mkdir(parents=True, exist_ok=True)
            source = vendor_root / text
            if not source.is_file():
                raise BuildError(f"{component['id']}: licence text {text} is missing")
            shutil.copy2(source, texts / source.name)


CUDA_VERSION_LINE = re.compile(r"Message: Found CUDA version = (\S+)")
GENCODE_LINE = re.compile(r"Message: CUDA gencode = (.*)")
GENCODE_CODE = re.compile(r"code=((?:sm|compute)_\d+)")


def cuda_targets(meson_log: str) -> dict:
    """The CUDA version and the cubin and PTX targets the build compiled its kernels
    for, as core/src/meson.build reports them in the Meson log."""
    version = CUDA_VERSION_LINE.search(meson_log)
    gencode = GENCODE_LINE.search(meson_log)
    if version is None or gencode is None:
        raise BuildError("the Meson log names no CUDA version or gencode list")
    codes = GENCODE_CODE.findall(gencode.group(1))
    targets = {
        "cuda_version": version.group(1),
        "cubins": sorted({c for c in codes if c.startswith("sm_")}, key=_arch_key),
        "ptx": sorted({c for c in codes if c.startswith("compute_")}, key=_arch_key),
    }
    if not targets["cubins"]:
        raise BuildError("the CUDA gencode list names no cubin")
    return targets


def _arch_key(code: str) -> int:
    return int(code.rsplit("_", 1)[1])


def meson_log(build_dir: Path) -> str:
    return (build_dir / "meson-logs" / "meson-log.txt").read_text(
        encoding="utf-8", errors="replace"
    )


def write_image_json(image_root: Path, name: str, document: dict) -> None:
    out = image_root / "image" / name
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")


def write_cuda_targets(build_dir: Path, image_root: Path) -> None:
    write_image_json(image_root, "cuda-targets.json", cuda_targets(meson_log(build_dir)))


HIP_TARGETS_LINE = re.compile(r"Message: HIP HSACO targets: (.*)")


def hip_targets(log: str, manifest: dict) -> dict:
    """The gfx targets the build compiled its code objects for, as core/src/meson.build
    reports them in the Meson log, and the ROCm release of TheRock's manifest."""
    line = HIP_TARGETS_LINE.search(log)
    targets = (
        sorted(set(re.findall(r"--offload-arch=(gfx[0-9a-f]+)", line.group(1)))) if line else []
    )
    if not targets:
        raise BuildError("the Meson log names no HIP offload target")
    return {"rocm_version": str(manifest.get("rocm_version", "unknown")),
            "therock_commit": str(manifest.get("the_rock_commit", "unknown")),
            "targets": targets}  # fmt: skip


def write_hip_targets(build_dir: Path, rocm_root: Path, image_root: Path) -> None:
    manifest_path = rocm_root / "share" / "therock" / "therock_manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise BuildError(f"cannot read {manifest_path}: {error}") from error
    write_image_json(image_root, "hip-targets.json", hip_targets(meson_log(build_dir), manifest))


def print_targets(build_dir: Path, names: list[str]) -> None:
    """Build targets of the listed tests that are executables of the build."""
    targets = {str(t["path"].relative_to(build_dir)) for t in native_tests(build_dir, names)}
    print("\n".join(sorted(targets)))


def run(argv: list[str]) -> int:
    command = argv[1] if len(argv) > 1 else ""
    if command == "select" and len(argv) == 4:
        print_targets(Path(argv[2]).resolve(), listed_names(Path(argv[3])))
    elif command == "stage" and len(argv) in (5, 6):
        stage(Path(argv[2]).resolve(), listed_names(Path(argv[3])), Path(argv[4]), *argv[5:])
    elif command == "fixtures" and len(argv) == 4:
        print("\n".join(report_fixture_lines(Path(argv[2]), Path(argv[3]))))
    elif command == "info" and len(argv) == 3:
        info(Path(argv[2]))
    elif command == "gate" and len(argv) == 4:
        stage_gate(Path(argv[2]).resolve(), Path(argv[3]))
    elif command == "twins" and len(argv) == 4:
        write_twins(Path(argv[2]), argv[3])
    elif command in ("intel-runtime", "rocm-runtime") and len(argv) == 5:
        stage_vendor_runtime(Path(argv[2]), Path(argv[3]), Path(argv[4]))
    elif command == "cuda-targets" and len(argv) == 4:
        write_cuda_targets(Path(argv[2]), Path(argv[3]))
    elif command == "hip-targets" and len(argv) == 5:
        write_hip_targets(Path(argv[2]), Path(argv[3]), Path(argv[4]))
    else:
        print(__doc__, file=sys.stderr)
        return 64
    return 0


def main(argv: list[str]) -> int:
    try:
        return run(argv)
    except BuildError as error:
        print(f"prepare_build: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
