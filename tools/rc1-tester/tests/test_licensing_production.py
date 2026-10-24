# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The licence gate applied to the production artifacts (ADR-1513): distroless
package records, PEP 639 licence directories, and the contracts that keep every
published production image behind its licence check."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
from pathlib import Path

import pytest

_IMAGE = Path(__file__).resolve().parents[1] / "image"
_spec = importlib.util.spec_from_file_location("licensing", _IMAGE / "licensing.py")
lic = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lic)

REPO = Path(__file__).resolve().parents[3]


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


# ------------------------------------------------------- distroless records


def distroless(tmp: Path) -> Path:
    """A distroless tree: package stanzas and md5sums in status.d, no info/*.list."""
    root = tmp / "root"
    write(root / "var/lib/dpkg/status.d/libc6", "Package: libc6\nSource: glibc\nVersion: 2.41-12\n")
    write(root / "var/lib/dpkg/status.d/libc6.md5sums",
          "00  usr/lib/x86_64-linux-gnu/libc.so.6\n00  usr/share/doc/libc6/copyright\n")  # fmt: skip
    write(root / "usr/lib/x86_64-linux-gnu/libc.so.6", "elf")
    write(root / "usr/share/doc/libc6/copyright", "LGPL-2.1+\n")
    os.symlink("libc.so.6", root / "usr/lib/x86_64-linux-gnu/libc.so")
    os.symlink("usr/lib", root / "lib")
    return root


def test_distroless_packages_and_their_files_are_read(tmp_path: Path) -> None:
    root = distroless(tmp_path)
    assert [p["Package"] for p in lic.dpkg_packages(root)] == ["libc6"]
    assert "usr/lib/x86_64-linux-gnu/libc.so.6" in lic.dpkg_owned(root)
    assert lic.package_sources(lic.dpkg_packages(root)[0]) == ["glibc=2.41-12"]


def test_a_link_onto_a_package_file_or_directory_is_claimed(tmp_path: Path) -> None:
    root = distroless(tmp_path)
    ctx = lic.Context(root, tmp_path, {"components": []})
    assert lic.dpkg_claims(ctx, "usr/lib/x86_64-linux-gnu/libc.so")
    assert lic.dpkg_claims(ctx, "lib")
    os.symlink("nowhere", root / "usr/lib/x86_64-linux-gnu/libdangling.so")
    os.symlink("../../../../etc/shadow", root / "usr/lib/x86_64-linux-gnu/libescape.so")
    assert not lic.dpkg_claims(ctx, "usr/lib/x86_64-linux-gnu/libdangling.so")
    assert not lic.dpkg_claims(ctx, "usr/lib/x86_64-linux-gnu/libescape.so")


def test_a_distroless_package_without_its_copyright_file_fails(tmp_path: Path) -> None:
    root = distroless(tmp_path)
    ctx = lic.Context(root, tmp_path, {"components": []})
    assert lic.check_dpkg(ctx) == []
    (root / "usr/share/doc/libc6/copyright").unlink()
    assert lic.check_dpkg(ctx) == ["package libc6 has no /usr/share/doc/*/copyright"]


# ------------------------------------------------------------ Python dists


def dist(root: Path, name: str, files: dict[str, str]) -> Path:
    info = root / f"site/{name}-1.0.dist-info"
    write(info / "METADATA", f"Metadata-Version: 2.4\nName: {name}\nVersion: 1.0\n"
          "License-Expression: EUPL-1.2\n")  # fmt: skip
    for rel, text in files.items():
        write(info / rel, text)
    write(info / "RECORD", "")
    return info


def py_record(texts: list[dict] | None = None) -> dict:
    component = {"id": "py", "kind": "python-dist", "name": "packages", "roots": ["site"]}
    if texts:
        component["texts"] = texts
    return {"components": [component]}


def test_a_pep639_licence_directory_counts_as_licence_files(tmp_path: Path) -> None:
    info = dist(tmp_path, "own", {"licenses/LICENSES/EUPL-1.2.txt": "EUPL text\n"})
    assert lic.dist_metadata(info)["licence_files"] == ["licenses/LICENSES/EUPL-1.2.txt"]
    ctx = lic.Context(tmp_path, tmp_path, py_record())
    assert lic.check_dists(ctx) == []


def test_a_dist_without_licence_file_needs_a_recorded_text(tmp_path: Path) -> None:
    dist(tmp_path, "bare", {})
    assert lic.check_dists(lic.Context(tmp_path, tmp_path, py_record())) == [
        "bare-1.0.dist-info keeps no licence file"
    ]
    recorded = py_record([{"fetched": "bare-LICENSE", "name": "bare-LICENSE.txt", "dist": "bare"}])
    assert lic.check_dists(lic.Context(tmp_path, tmp_path, recorded)) == []
    other = py_record([{"fetched": "x", "name": "x.txt", "dist": "another"}])
    assert lic.check_dists(lic.Context(tmp_path, tmp_path, other)) != []


# ------------------------------------------------------------- contracts


def published_targets(workflow: str) -> set[tuple[str, str]]:
    """(Dockerfile, target) of every image a publish workflow pushes."""
    text = (REPO / ".github/workflows" / workflow).read_text()
    pairs = re.findall(r"file: (\S+)\n\s+target: (\S+)", text)
    return {(f, t) for f, t in pairs if not t.endswith("source-export")}


def final_stage(dockerfile: str, target: str) -> str:
    text = (REPO / dockerfile).read_text()
    match = re.search(
        rf"^FROM \S+ AS {re.escape(target)}\n(.*?)(?=^FROM |\Z)", text, re.MULTILINE | re.DOTALL
    )
    assert match, f"{dockerfile} has no target {target}"
    return match.group(1)


def has_licence_gate(dockerfile: str, target: str) -> bool:
    stage = final_stage(dockerfile, target)
    gate = r"^COPY --from=\S*licence-check /out/licence-check.json"
    return re.search(gate, stage, re.MULTILINE) is not None


@pytest.mark.parametrize(
    "workflow", ["docker-publish-production.yml", "docker-publish-operator-node.yml"]
)
def test_every_published_production_target_passes_a_licence_check(workflow: str) -> None:
    targets = published_targets(workflow)
    assert targets
    for dockerfile, target in sorted(targets):
        assert has_licence_gate(
            dockerfile, target
        ), f"{dockerfile} target {target} is published without a licence check"


def test_the_gate_detector_refuses_a_target_without_the_receipt(tmp_path: Path) -> None:
    """has_licence_gate() is the whole contract above, so it must see a missing receipt."""
    stage = final_stage("docker/Dockerfile.production-gpu", "final-rocm10")
    assert "COPY --from=rocm10-licence-check /out/licence-check.json" in stage
    assert has_licence_gate("docker/Dockerfile.production-gpu", "final-rocm10")
    ungated = (
        (REPO / "docker/Dockerfile.production-gpu")
        .read_text()
        .replace(
            "COPY --from=rocm10-licence-check /out/licence-check.json", "COPY licence-check.json"
        )
    )
    write(tmp_path / "Dockerfile", ungated)
    assert not has_licence_gate(str(tmp_path / "Dockerfile"), "final-rocm10")


@pytest.mark.parametrize(("dockerfile", "kind", "source"), [
    ("docker/Dockerfile.production", "production-cli-image", "cli-source-export"),
    ("docker/Dockerfile.production", "production-server-image", "server-source-export"),
    ("docker/Dockerfile.production-gpu", "production-cuda-image", "cuda13-source-export"),
    ("docker/Dockerfile.production-gpu", "production-rocm-image", "rocm10-source-export"),
    ("docker/Dockerfile.production-gpu", "production-oneapi-image", "oneapi2026-source-export"),
])  # fmt: skip
def test_the_production_images_carry_notices_and_publish_their_source(
    dockerfile: str, kind: str, source: str
) -> None:
    text = (REPO / dockerfile).read_text()
    assert f"licensing.py notices --artifact {kind}" in text
    assert f"licensing.py check --artifact {kind}" in text
    assert f"FROM scratch AS {source}\n" in text
    workflow = (REPO / ".github/workflows/docker-publish-production.yml").read_text()
    assert f"source-target: {source}\n" in workflow
    assert "VMAFX_SOURCE_COMMIT=${{ needs.validate-release.outputs.source-sha }}" in workflow


def test_the_licence_artifacts_action_attests_spdx_with_the_pinned_tools() -> None:
    action = (REPO / ".github/actions/image-licence-artifacts/action.yml").read_text()
    assert "syft-version: v1.51.1" in action
    assert action.count("actions/attest@1e69f48acb82d1966a394da916b4c1698aa569d6") == 2
    assert "sbom-path: sbom-${{ inputs.name }}-amd64.spdx.json" in action
    assert "attest-sbom" not in action  # deprecated (ADR-1503 rule 6)


@pytest.mark.parametrize(("dockerfile", "kinds"), [
    ("docker/Dockerfile.production", ["production-cli-image", "production-server-image"]),
    ("docker/Dockerfile.production-gpu",
     ["production-cuda-image", "production-rocm-image", "production-oneapi-image"]),
])  # fmt: skip
def test_the_image_licence_label_is_the_compiled_licence_set(dockerfile: str, kinds: list) -> None:
    records = lic.load_manifest()["artifacts"]
    expected = set()
    for kind in kinds:
        binaries = next(c for c in records[kind]["components"] if c["id"] == "vmafx-binaries")
        expected.add(" AND ".join(binaries["licences"]))
    labels = re.findall(r'org\.opencontainers\.image\.licenses="([^"]*)"',
                        (REPO / dockerfile).read_text())  # fmt: skip
    assert labels and set(labels) == expected


def test_the_production_records_name_every_text_they_fetch() -> None:
    data = lic.load_manifest()
    for kind in ("production-cli-image", "production-server-image"):
        record = data["artifacts"][kind]
        assert lic.recorded_fetches(record, data).keys() <= data["fetched_texts"].keys()
        assert json.dumps(record).count("vmafx-binaries") == 1
    assert (
        data["artifacts"]["production-server-image"]["python"]["version"]
        in data["cpython_license_rst"]
    )


# ------------------------------------------------------ shared components


def shared_manifest() -> dict:
    vendor = {"id": "vendor", "kind": "fixed", "name": "a vendor runtime",
              "paths": ["opt/vmafx/lib/v/libv.so*"], "licence": "MIT",
              "texts": [{"artifact": "opt/vmafx/licenses/v/LICENSE", "label": "its licence"}],
              "notes": ["shipped in opt/vmafx/lib/v/"]}  # fmt: skip
    return {"artifacts": {
        "tester": {"components": [vendor]},
        "prod": {"rewrite": {"opt/vmafx/lib/": "usr/local/lib/",
                             "opt/vmafx/licenses/": "usr/local/share/vmafx/licenses/"},
                 "components": [{"from": "tester", "id": "vendor", "name": "renamed"}]},
    }}  # fmt: skip


def test_a_shared_component_is_rewritten_and_overridden() -> None:
    expanded = lic.expand_shared(shared_manifest())["artifacts"]
    component = expanded["prod"]["components"][0]
    assert component["paths"] == ["usr/local/lib/v/libv.so*"]
    assert component["texts"][0]["artifact"] == "usr/local/share/vmafx/licenses/v/LICENSE"
    assert component["notes"] == ["shipped in usr/local/lib/v/"]
    assert component["name"] == "renamed" and "from" not in component
    assert expanded["tester"]["components"][0]["paths"] == ["opt/vmafx/lib/v/libv.so*"]


def test_a_shared_component_that_does_not_exist_is_refused() -> None:
    data = shared_manifest()
    data["artifacts"]["prod"]["components"][0]["id"] = "gone"
    with pytest.raises(lic.LicensingError, match="no component 'gone'"):
        lic.expand_shared(data)


def test_a_shared_component_never_names_another_shared_one() -> None:
    data = shared_manifest()
    data["artifacts"]["third"] = {"components": [{"from": "prod", "id": "vendor"}]}
    with pytest.raises(lic.LicensingError, match="of its own"):
        lic.expand_shared(data)


def test_the_gpu_records_share_the_tester_vendor_components() -> None:
    """The production GPU images ship the vendor runtime files of the tester images;
    their record takes those components, not copies of them (ADR-1517)."""
    raw = json.loads((_IMAGE / "licensing.json").read_text())["artifacts"]
    expanded = lic.load_manifest()["artifacts"]
    for kind, tester in (("production-cuda-image", "cuda-image"),
                         ("production-rocm-image", "hip-image"),
                         ("production-oneapi-image", "sycl-image")):  # fmt: skip
        shared = {c["id"] for c in raw[kind]["components"] if c.get("from") == tester}
        tester_only = {"netflix-test-videos", "brisque-live-model", "build-records"}
        vendor = {c["id"] for c in raw[tester]["components"]
                  if c["kind"] in {"fixed", "dpkg-foreign"} and c["id"] not in tester_only}  # fmt: skip
        assert vendor and vendor <= shared, (kind, vendor - shared)
        text = json.dumps(expanded[kind]["components"])
        assert "opt/vmafx/" not in text, kind


def test_notices_of_a_distroless_tree_list_its_packages(tmp_path: Path) -> None:
    root = distroless(tmp_path)
    repo = tmp_path / "repo"
    write(repo / "REUSE.toml", "version = 1\n")
    scan_file = write(tmp_path / "scan.json", json.dumps({"licences": [], "files": []}))
    (tmp_path / "texts").mkdir()
    record = {"title": "t", "licence_root": "licenses", "source_offer": "{tag}-source",
              "components": [{"id": "deb", "kind": "dpkg", "name": "packages"},
                             {"id": "state", "kind": "state", "name": "s",
                              "paths": ["var/lib/dpkg/status.d/**"]},
                             {"id": "notices", "kind": "notices", "name": "n", "paths": ["licenses/**"]}]}  # fmt: skip
    data = {
        "spdx_texts": {},
        "grafted_libraries": [],
        "source_archives": {},
        "artifacts": {"k": record},
    }
    args = argparse.Namespace(artifact="k", root=str(root), repo=str(repo), build_scan=str(scan_file),
                              texts=str(tmp_path / "texts"), source_commit="c0ffee", tag="t1",
                              python_version="none", receipt=None)  # fmt: skip
    lic.write_notices(args, data)
    assert (
        "  libc6 2.41-12  source glibc=2.41-12"
        in (root / "licenses/THIRD_PARTY_NOTICES.txt").read_text()
    )
    assert lic.run_check(args, data) == []
    assert lic.source_list(args, data) == ["debian glibc=2.41-12"]


def test_model_annotations_match_the_registry() -> None:
    """Every tiny model's weights carry, through REUSE.toml, the licence its registry
    entry declares, so the computed notices name the upstream holder (ADR-1513)."""
    registry = json.loads((REPO / "model/tiny/registry.json").read_text())
    reuse = lic.load_reuse(REPO)
    for model in registry["models"]:
        rel = f"model/tiny/{model['onnx']}"
        expression, copyrights = lic.file_licence(REPO / rel, rel, reuse)
        assert lic.spdx_ids(model["license"]) <= lic.spdx_ids(expression), (rel, expression)
        assert copyrights, rel
    for rel in ("model/predictor_libx264.onnx", "model/konvid_mos_head_v1.onnx"):
        assert lic.file_licence(REPO / rel, rel, reuse)[1] == ["Copyright 2026 Lusoris"], rel


def test_the_release_files_carry_checked_notices() -> None:
    """The native release bundle writes and checks the notices of models.tar.gz
    and of the release files, and the release attaches them and attests SBOMs."""
    script = (REPO / "scripts/release/build-native-release-artifacts.sh").read_text()
    assert "write_and_check_notices release-models" in script
    assert "write_and_check_notices release-native" in script
    main = script.split("\nmain() {", 1)[1]
    assert main.index("stage_licences") < main.index("verify-native-release-artifacts.sh")
    assert main.index("--stamp artifacts") < main.index("stage_licences")  # the stamp is recorded
    workflow = (REPO / ".github/workflows/supply-chain.yml").read_text()
    for asset in ("THIRD_PARTY_NOTICES.txt", "licenses.tar.gz"):
        assert f"dist/release-artifacts/{asset}" in workflow
    for sbom in ("vmafx.spdx.json", "vmaf-mcp.spdx.json"):
        assert f"sbom-path: sbom/{sbom}" in workflow
    records = lic.load_manifest()["artifacts"]
    assert {"release-native", "release-models"} <= records.keys()


def test_production_node_sycl_image_shares_runtime_and_node_components() -> None:
    """production-node-sycl-image shares the oneAPI SYCL runtime components and the
    Intel GPU user-mode stack from sycl-image and the worker node components from production-node-image (ADR-3371).
    """
    raw = json.loads((_IMAGE / "licensing.json").read_text())["artifacts"]
    assert "production-node-sycl-image" in raw
    sycl_node = raw["production-node-sycl-image"]
    shared_sycl = {c["id"] for c in sycl_node["components"] if c.get("from") == "sycl-image"}
    assert shared_sycl == {
        "intel-oneapi-dpcpp-runtime",
        "intel-oneapi-umf",
        "intel-oneapi-tcm-hwloc",
        "intel-gpu-stack",
    }
    shared_node = {
        c["id"] for c in sycl_node["components"] if c.get("from") == "production-node-image"
    }
    for required in (
        "vmafx-binaries",
        "ffmpeg",
        "svt-av1",
        "go-modules",
        "rclone",
        "build-records",
    ):
        assert required in shared_node, f"missing {required} in shared_node"

    expanded = lic.load_manifest()["artifacts"]
    assert "production-node-sycl-image" in expanded
    text = json.dumps(expanded["production-node-sycl-image"]["components"])
    assert "opt/vmafx/" not in text
