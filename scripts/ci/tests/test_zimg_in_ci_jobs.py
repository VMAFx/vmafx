# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A CI job that runs ``meson setup`` carries zimg, which the default build requires."""

from __future__ import annotations

import re
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[3]
WORKFLOWS = REPO / ".github" / "workflows"
APT_LISTS = REPO / ".github" / "apt"
MATRIX = WORKFLOWS / "libvmaf-build-matrix.yml"
ZIMG_SOURCES = (
    "libzimg-dev",
    "brew install",
    "setup-zimg-windows.ps1",
    "scripts/setup/ubuntu.sh",
    "-zimg",
)
# The image of these self-hosted runners is built from dev/Containerfile.
RUNNER_IMAGE_JOBS = {
    ("sycl-parity.yml", "sycl-parity"),
    ("tests-and-quality-gates.yml", "coverage-gpu"),
}


def step_text(step: dict) -> str:
    return "\n".join(str(step.get(key, "")) for key in ("run", "uses", "with"))


def apt_lists_with_zimg() -> list[str]:
    return [
        f".github/apt/{path.name}"
        for path in sorted(APT_LISTS.glob("*.txt"))
        if "libzimg-dev" in path.read_text(encoding="utf-8").splitlines()
    ]


def job_carries_zimg(text: str, lists: list[str]) -> bool:
    return any(source in text for source in (*ZIMG_SOURCES, *lists))


def jobs_without_zimg() -> list[tuple[str, str]]:
    lists = apt_lists_with_zimg()
    missing = []
    for path in sorted(WORKFLOWS.glob("*.yml")):
        jobs = yaml.safe_load(path.read_text(encoding="utf-8")).get("jobs") or {}
        for name, job in jobs.items():
            text = "\n".join(step_text(step) for step in job.get("steps") or [])
            if "meson setup" in text and not job_carries_zimg(text, lists):
                missing.append((path.name, name))
    return missing


def apt_steps_without_zimg(steps: list[dict]) -> list[str]:
    return [
        str(step.get("name"))
        for step in steps
        if "apt-get -yq install" in str(step.get("run", ""))
        and "ninja-build" in str(step.get("run", ""))
        and "libzimg-dev" not in str(step.get("run", ""))
    ]


def test_every_meson_job_carries_zimg() -> None:
    assert set(jobs_without_zimg()) == RUNNER_IMAGE_JOBS


def test_the_runner_image_carries_zimg() -> None:
    text = (REPO / "dev" / "Containerfile").read_text(encoding="utf-8")
    assert "libzimg-dev" in text
    runner = (REPO / "dev" / "Containerfile.runner").read_text(encoding="utf-8")
    assert "vmaf-dev-mcp" in runner


def test_a_job_without_zimg_is_found() -> None:
    assert not job_carries_zimg("meson setup build core\nsudo apt-get install gcc", [])
    assert job_carries_zimg("grep .github/apt/x.txt", [".github/apt/x.txt"])


def test_every_matrix_install_step_carries_zimg() -> None:
    workflow = yaml.safe_load(MATRIX.read_text(encoding="utf-8"))
    assert apt_steps_without_zimg(workflow["jobs"]["libvmaf-build"]["steps"]) == []


def test_a_matrix_install_step_without_zimg_is_found() -> None:
    step = {"name": "base", "run": "sudo -E apt-get -yq install ccache ninja-build nasm gcc g++"}
    assert apt_steps_without_zimg([step]) == ["base"]


def stages(text: str) -> dict[str, str]:
    """Stage name to its own text and its base stage, in file order."""
    found: dict[str, str] = {}
    parts = re.split(r"(?m)^(?=FROM\s)", text)
    for part in parts[1:]:
        header = part.splitlines()[0].split()
        name = header[-1] if "AS" in header else f"unnamed-{len(found)}"
        inherited = found.get(header[1], "")
        code = (line for line in part.splitlines() if not line.lstrip().startswith("#"))
        found[name] = "\n".join((inherited, *code))
    return found


def ffmpeg_stages_without_zimg(text: str) -> list[str]:
    return [
        name
        for name, body in stages(text).items()
        if "--enable-libvmaf" in body and "libzimg-dev" not in body
    ]


def test_every_ffmpeg_stage_can_resolve_libvmaf() -> None:
    """FFmpeg's configure asks pkg-config for libvmaf, whose Requires names zimg."""
    for name in (
        "Dockerfile",
        "Containerfile.vmafx",
        "dev/Containerfile",
        "docker/Dockerfile.node",
    ):
        text = (REPO / name).read_text(encoding="utf-8")
        assert "--enable-libvmaf" in text, name
        assert ffmpeg_stages_without_zimg(text) == [], name


def test_an_ffmpeg_stage_without_zimg_is_found() -> None:
    text = "FROM a AS deps\nRUN apt-get install gcc\nFROM deps AS ff\nRUN ./configure --enable-libvmaf\n"
    assert ffmpeg_stages_without_zimg(text) == ["ff"]
    fixed = text.replace("gcc", "libzimg-dev")
    assert ffmpeg_stages_without_zimg(fixed) == []
