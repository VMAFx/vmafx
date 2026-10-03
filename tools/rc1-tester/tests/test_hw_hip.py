# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the HIP backend of the GPU section (hw_hip.py, hw_hipprobe.py) and for
the build helper that records the build's gfx targets. Device-free: every runner is
a fake, every device node a file in a temporary directory."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester import hw_gpu, hw_hip, hw_hipprobe
from vmaf_rc1_tester.safe_process import CommandResult

_spec = importlib.util.spec_from_file_location(
    "prepare_build", _HERE.parent / "image" / "prepare_build.py"
)
prepare_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(prepare_build)

MESON_LOG = ("Message: HIP HSACO targets: --offload-arch=gfx90a --offload-arch=gfx1036 "
             "--offload-arch=gfx1100 --offload-arch=gfx1201\n")  # fmt: skip
MANIFEST = {"rocm_version": "10.0.0", "the_rock_commit": "16adc4d8"}
TARGETS = prepare_build.hip_targets(MESON_LOG, MANIFEST)
FIXTURE = {"id": "f1", "ref": "r.yuv", "dis": "d.yuv", "width": 16, "height": 16,
           "pixel_format": "420", "bitdepth": 8}  # fmt: skip
EXACT = {"metrics": ["psnr_y"], "extractor": "psnr_hip", "options": "", "bound": "0",
         "source": "exact:ADR-1497"}  # fmt: skip
BUDGET = hw_gpu.Budget(fixture=10, gate=10, tests=10)
IGPU = {"index": 0, "name": "AMD Radeon Graphics", "gfx_target": "gfx1036", "family": "rdna2",
        "compute_units": 2}  # fmt: skip
OPEN = {"path": "kfd", "kfd": {"present": True, "accessible": True, "gid": 993},
        "render_nodes": [], "dxg_present": False}  # fmt: skip


# ---- build targets -------------------------------------------------------------------------


def test_hip_targets_come_from_the_meson_log_and_the_rock_manifest(tmp_path: Path) -> None:
    assert TARGETS == {"rocm_version": "10.0.0", "therock_commit": "16adc4d8",
                       "targets": ["gfx1036", "gfx1100", "gfx1201", "gfx90a"]}  # fmt: skip
    with pytest.raises(prepare_build.BuildError, match="no HIP offload target"):
        prepare_build.hip_targets("Message: CUDA gencode = []\n", MANIFEST)
    build, rocm = tmp_path / "build", tmp_path / "rocm"
    (build / "meson-logs").mkdir(parents=True)
    (build / "meson-logs" / "meson-log.txt").write_text(MESON_LOG)
    argv = ["prepare_build.py", "hip-targets", str(build), str(rocm), str(tmp_path)]
    assert prepare_build.main(argv) == 1  # no TheRock manifest: refused
    (rocm / "share" / "therock").mkdir(parents=True)
    (rocm / "share" / "therock" / "therock_manifest.json").write_text(json.dumps(MANIFEST))
    assert prepare_build.main(argv) == 0
    assert json.loads((tmp_path / "image" / "hip-targets.json").read_text()) == TARGETS


# ---- hw_hipprobe ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("target", "family"),
    [
        ("gfx908", "cdna1"),
        ("gfx90a", "cdna2"),
        ("gfx942", "cdna3"),
        ("gfx950", "cdna4"),
        ("gfx1012", "rdna1"),
        ("gfx1030", "rdna2"),
        ("gfx1036", "rdna2"),  # the Raphael iGPU of ryzen-4090-arc
        ("gfx1100", "rdna3"),
        ("gfx1103", "rdna3"),
        ("gfx1151", "rdna3.5"),
        ("gfx1201", "rdna4"),
        ("gfx906", "unknown"),  # boundary: GCN5, no family the build targets
        ("gfx1250", "unknown"),
        ("AMD Ryzen 9", "unknown"),  # a CPU agent's name
    ],
)
def test_gfx_target_families(target, family) -> None:
    assert hw_hipprobe.family_of(target) == family


def test_probe_without_the_runtime_library_reports_it() -> None:
    result = hw_hipprobe.probe("libhsa-runtime64_absent.so.9")
    assert result["status"] == "no_runtime_library" and result["devices"] == []


# ---- access --------------------------------------------------------------------------------


def amd_node(dri: Path, sysfs: Path, name: str, mode: int = 0o666) -> None:
    node = dri / name
    node.write_text("")
    node.chmod(mode)
    device = sysfs / name / "device"
    device.mkdir(parents=True)
    (device / "vendor").write_text("0x1002\n")
    (device / "device").write_text("0x13c0\n")
    (device.parent / "amdgpu").mkdir()
    (device / "driver").symlink_to(device.parent / "amdgpu")


def test_access_needs_kfd_and_an_amd_render_node(tmp_path: Path) -> None:
    dri, sysfs, kfd = tmp_path / "dri", tmp_path / "sys", tmp_path / "kfd"
    dri.mkdir()
    amd_node(dri, sysfs, "renderD128")
    facts = hw_hip.access_facts(kfd, dri, tmp_path / "dxg", sysfs)
    assert facts["path"] == "none" and "--device /dev/kfd" in hw_hip.missing_access_reason(facts)
    kfd.write_text("")
    kfd.chmod(0o666)
    facts = hw_hip.access_facts(kfd, dri, tmp_path / "dxg", sysfs)
    assert facts["path"] == "kfd" and facts["render_nodes"][0]["driver"] == "amdgpu"


def test_reasons_name_groups_wsl_and_missing_nodes(tmp_path: Path) -> None:
    closed = {"kfd": {"present": True, "accessible": False, "gid": 993}, "dxg_present": False,
              "render_nodes": [{"vendor": "0x1002", "accessible": False, "gid": 110}]}  # fmt: skip
    assert hw_hip.missing_access_reason(closed).endswith("--group-add 110 --group-add 993")
    wsl = {"kfd": {"present": False, "accessible": False, "gid": None}, "dxg_present": True,
           "render_nodes": []}  # fmt: skip
    assert "WSL2" in hw_hip.missing_access_reason(wsl) and "Linux" in hw_hip.missing_access_reason(
        wsl
    )
    no_render = {"kfd": {"present": True, "accessible": True, "gid": 993}, "dxg_present": False,
                 "render_nodes": [{"vendor": "0x8086", "accessible": True, "gid": 110}]}  # fmt: skip
    assert "PCI vendor 0x1002" in hw_hip.missing_access_reason(no_render)


# ---- discovery and pinning -----------------------------------------------------------------


def probe_runner(document: dict, seen: list):
    def run(argv, **kwargs):
        seen.append((argv, kwargs["environment"]))
        return CommandResult(0, json.dumps(document), "")

    return run


def fake_root(tmp_path: Path) -> Path:
    (tmp_path / "image").mkdir(parents=True, exist_ok=True)
    (tmp_path / "image" / "hip-targets.json").write_text(json.dumps(TARGETS))
    return tmp_path


def test_discover_leaves_out_a_gpu_without_a_code_object(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setattr(hw_hip, "access_facts", lambda: dict(OPEN))
    rx6600 = {"index": 1, "name": "AMD Radeon RX 6600", "gfx_target": "gfx1032", "family": "rdna2"}
    document = {"status": "ok", "hsa_runtime_version": "1.21", "devices": [IGPU, rx6600]}
    found = hw_hip.discover(fake_root(tmp_path), probe_runner(document, []))
    assert [d["index"] for d in found["devices"]] == [0]
    assert found["runtime"] == {"rocm_version": "10.0.0", "hsa_runtime_version": "1.21"}
    assert "no code object for gfx1032" in found["access"]["devices_left_out"][0]["reason"]
    alone = hw_hip.discover(
        fake_root(tmp_path), probe_runner({"status": "ok", "devices": [rx6600]}, [])
    )
    assert alone["devices"] == [] and "no code object for gfx1032" in alone["reason"]


def test_discover_does_not_start_the_runtime_without_access(tmp_path: Path, monkeypatch) -> None:
    closed = {**OPEN, "path": "none", "kfd": {"present": False, "accessible": False, "gid": None}}
    monkeypatch.setattr(hw_hip, "access_facts", lambda: dict(closed))
    seen: list = []
    found = hw_hip.discover(fake_root(tmp_path), probe_runner({"devices": [IGPU]}, seen))
    assert seen == [] and found["devices"] == [] and "--device /dev/kfd" in found["reason"]


def test_device_env_pins_one_gpu_agent() -> None:
    assert hw_hip.device_env({"index": 1}) == {"ROCR_VISIBLE_DEVICES": "1"}


# ---- the whole section with the HIP backend ------------------------------------------------


def section_image(root: Path) -> None:
    fake_root(root)
    (root / "image" / "gpu-twins.json").write_text(
        json.dumps({"backend": "hip", "features": {"psnr": EXACT}})
    )
    (root / "image" / "hip-rows.json").write_text(json.dumps({"rows": [
        {"id": "T-X-2026-10-03", "part": "RDNA2", "families": ["rdna2"], "tests": ["test_a"]},
        {"id": "T-X-2026-10-03", "part": "RDNA4", "families": ["rdna4"], "tests": ["test_a"]},
    ]}))  # fmt: skip
    (root / "image" / "gpu-tests.json").write_text(
        json.dumps({"tests": [{"name": "test_a", "cmd": "/t/test_a"}], "left_out": []})
    )


def section_runner(device_value: float, seen: list):
    def run(argv, **kwargs):
        seen.append((argv, kwargs.get("environment", {})))
        if "hw_hipprobe" in " ".join(argv):
            return CommandResult(0, json.dumps({"status": "ok", "devices": [IGPU]}), "")
        if "--output" in argv:
            document = {"frames": [{"frameNum": 0, "metrics": {"psnr_y": device_value}}],
                        "feature_backends": [{"extractor": "psnr_hip", "backend": "hip"}]}  # fmt: skip
            Path(argv[argv.index("--output") + 1]).write_text(json.dumps(document))
            return CommandResult(0, "", "")
        return CommandResult(0 if argv[0] == "/t/test_a" else 1, "", "")

    return run


def test_section_pins_every_run_and_applies_the_family_rows(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setattr(hw_hip, "access_facts", lambda: dict(OPEN))
    section_image(tmp_path)
    seen: list = []
    section = hw_gpu.run_gpu_section(tmp_path, hw_hip.HIP, [FIXTURE], {"f1": {"psnr_y": [40.0]}},
                                     BUDGET, runner=section_runner(40.0, seen))  # fmt: skip
    device = section["devices"][0]
    assert device["twins"]["status"] == "identical" and device["audits"] == {}
    runs = [env for argv, env in seen if "hw_hipprobe" not in " ".join(argv)]
    assert runs and all(env.get("ROCR_VISIBLE_DEVICES") == "0" for env in runs)
    rows = {row["part"]: row["verdict"] for row in device["rows"]["rows"]}
    assert rows == {"RDNA2": "pass", "RDNA4": "not_applicable"}
    assert "(rdna2 gfx1036)" in hw_gpu.summary_lines(section)[1]


def test_planted_wrong_cpu_value_fails_closed_and_names_the_frame(
    tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setattr(hw_hip, "access_facts", lambda: dict(OPEN))
    section_image(tmp_path)
    section = hw_gpu.run_gpu_section(
        tmp_path, hw_hip.HIP, [FIXTURE], {"f1": {"psnr_y": [40.000000000000007]}}, BUDGET,
        runner=section_runner(40.0, []),
    )  # fmt: skip
    feature = section["devices"][0]["twins"]["features"][0]
    assert feature["status"] == "differing" and section["status"] == "fail"
    assert feature["first"]["frame"] == 0 and feature["first"]["cpu"] == "40.000000000000007"


def test_no_device_is_no_device_with_the_missing_option(tmp_path: Path, monkeypatch) -> None:
    closed = {**OPEN, "path": "none", "kfd": {"present": False, "accessible": False, "gid": None}}
    monkeypatch.setattr(hw_hip, "access_facts", lambda: dict(closed))
    section_image(tmp_path)
    section = hw_gpu.run_gpu_section(tmp_path, hw_hip.HIP, [FIXTURE], {}, BUDGET,
                                     runner=probe_runner({}, []))  # fmt: skip
    assert section["status"] == "no_device" and "--device /dev/kfd" in section["reason"]
    assert hw_gpu.gpu_not_exercised(section) == [("HIP twins", section["reason"])]
