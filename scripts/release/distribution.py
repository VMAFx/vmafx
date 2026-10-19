#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Write every distribution channel's metadata from packaging/manifest.toml, and check it.

``check`` fails when the manifest disagrees with the tree: a licence that is not what
the artifact's licensing record or package manifest says, a licence without its text in
LICENSES/, a producer job its workflow does not define, or a channel that names an
artifact no pipeline builds. ``generate`` writes the Homebrew formula, the conda recipe,
the three winget manifests and one JSON index per channel for a release version.
Guide: docs/development/distribution.md.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

import tomllib
import yaml  # type: ignore[import-untyped]

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.ci import check_licence_metadata as licence_check  # noqa: E402

MANIFEST = Path("packaging") / "manifest.toml"
BACKENDS = ("cpu", "cuda", "sycl", "hip", "metal")
FEATURE_OPTIONS = ("metal",)
KINDS = ("native", "tester-bundle", "python-wheel", "python-sdist", "crate", "source")
ARTIFACT_KEYS = ("kind", "platform", "abi", "backends", "licence", "files", "signing", "sbom")
STATES = ("published", "planned")
WINGET_SCHEMA = "1.12.0"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$")


class DistributionError(Exception):
    """A manifest or an argument this tool cannot use."""


def load(root: Path) -> dict[str, Any]:
    try:
        return tomllib.loads((root / MANIFEST).read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise DistributionError(f"{MANIFEST}: {error}") from error


# ------------------------------------------------------------------------------- check


def shape_problems(name: str, artifact: dict[str, Any]) -> list[str]:
    """Keys every artifact needs and the values they may take."""
    problems = [f"{name}: no {key}" for key in ARTIFACT_KEYS if key not in artifact]
    if artifact.get("kind") not in KINDS:
        problems.append(f"{name}: kind {artifact.get('kind')!r} is not one of {KINDS}")
    unknown = sorted(set(artifact.get("backends", [])) - set(BACKENDS))
    if unknown:
        problems.append(f"{name}: unknown backends {unknown}")
    if ("licence_record" in artifact) == ("package" in artifact):
        problems.append(f"{name}: needs exactly one of licence_record and package")
    if ("producer" in artifact) == ("tracking" in artifact):
        problems.append(f"{name}: needs exactly one of producer and tracking")
    return problems


def record_licences(root: Path, record: str) -> set[str]:
    """The licences the build and repository components of a licensing record carry."""
    records = licence_check.licensing.load_manifest(root / licence_check.LICENSING_MANIFEST)
    try:
        components = records["artifacts"][record]["components"]
    except KeyError as error:
        raise DistributionError(f"no licensing record {record!r}") from error
    found: set[str] = set()
    for component in components:
        if component.get("kind") in ("build", "repo"):
            found |= set(component.get("licences", []))
    return found


def package_licences(root: Path, package: str) -> set[str]:
    """The licences the package manifest's own licence field names."""
    path = root / package
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    if path.name == "pyproject.toml":
        declared = data.get("project", {}).get("license")
    else:
        workspace = licence_check._cargo_workspace(root, path)
        declared = licence_check._cargo_field(data.get("package", {}), "license", workspace)
    if not isinstance(declared, str):
        raise DistributionError(f"{package}: no licence expression")
    ids: set[str] = licence_check.licensing.spdx_ids(declared)
    return ids


def licence_problems(root: Path, name: str, artifact: dict[str, Any]) -> list[str]:
    """The artifact's licence is its source's, and every licence has its text."""
    if "licence_record" in artifact:
        expected = record_licences(root, artifact["licence_record"])
    else:
        expected = package_licences(root, artifact["package"])
    problems = licence_check.declared_problems(name, "licence", artifact["licence"], expected)
    for identifier in sorted(licence_check.licensing.spdx_ids(artifact["licence"])):
        if not (root / "LICENSES" / f"{identifier}.txt").is_file():
            problems.append(f"{name}: no LICENSES/{identifier}.txt")
    return problems


def job_pattern(template: str) -> re.Pattern[str]:
    """A job name template with each ${{ ... }} expression standing for any text."""
    parts = re.split(r"\$\{\{[^}]*\}\}", template)
    return re.compile("^" + ".+".join(re.escape(part) for part in parts) + "$")


def producer_problems(root: Path, name: str, producer: dict[str, Any]) -> list[str]:
    """The workflow exists and defines a job of that name."""
    if producer.get("forge") == "tag-archive":
        return []
    workflow = root / ".github" / "workflows" / str(producer.get("workflow", ""))
    if not workflow.is_file():
        return [f"{name}: producer workflow {producer.get('workflow')!r} does not exist"]
    jobs = (yaml.safe_load(workflow.read_text(encoding="utf-8")) or {}).get("jobs", {})
    templates = [str(job.get("name", key)) for key, job in jobs.items()]
    if any(job_pattern(t).match(str(producer.get("job"))) for t in templates):
        return []
    return [f"{name}: {workflow.name} defines no job {producer.get('job')!r}"]


def channel_problems(name: str, channel: dict[str, Any], artifacts: dict[str, Any]) -> list[str]:
    """A channel carries only artifacts a pipeline builds; a planned one names its issue."""
    problems = []
    if channel.get("state") not in STATES:
        problems.append(f"channel {name}: state {channel.get('state')!r} is not one of {STATES}")
    if (channel.get("state") == "planned") != ("tracking" in channel):
        problems.append(
            f"channel {name}: a planned channel, and only one, names its tracking issue"
        )
    for key in channel.get("artifacts", []):
        if key not in artifacts:
            problems.append(f"channel {name}: artifact {key!r} is not in the manifest")
        elif "producer" not in artifacts[key]:
            tracking = artifacts[key].get("tracking", "no issue")
            problems.append(f"channel {name}: artifact {key!r} is not built ({tracking})")
    return problems


def check(root: Path, manifest: dict[str, Any]) -> list[str]:
    if manifest.get("schema_version") != 1:
        return [f"{MANIFEST}: schema_version must be 1"]
    artifacts = manifest.get("artifacts", {})
    problems: list[str] = []
    for name, artifact in artifacts.items():
        shape = shape_problems(name, artifact)
        problems += shape
        if not shape:
            problems += licence_problems(root, name, artifact)
            if "producer" in artifact:
                problems += producer_problems(root, name, artifact["producer"])
    for name, channel in manifest.get("channels", {}).items():
        problems += channel_problems(name, channel, artifacts)
    return problems


# ---------------------------------------------------------------------------- generate


def pep440(version: str) -> str:
    """The PEP 440 spelling scripts/release/pep440-version.sh gives a SemVer tag."""
    match = re.fullmatch(r"([0-9.]+)(?:-(alpha|beta|rc)\.([0-9]+))?", version)
    if match is None:
        raise DistributionError(f"version {version!r} has no PEP 440 spelling")
    core, label, number = match.groups()
    short = {"alpha": "a", "beta": "b", "rc": "rc"}
    return core if label is None else f"{core}{short[label]}{number}"


def file_names(artifact: dict[str, Any], version: str) -> list[str]:
    spelling = {"version": version, "pep440": pep440(version)}
    return [name.format(**spelling) for name in artifact["files"]]


def source_url(manifest: dict[str, Any], version: str) -> str:
    return f"{manifest['project']['repository']}/archive/refs/tags/v{version}.tar.gz"


def release_url(manifest: dict[str, Any], version: str, name: str) -> str:
    return f"{manifest['project']['repository']}/releases/download/v{version}/{name}"


def configure_flags(artifact: dict[str, Any]) -> list[str]:
    """The Meson options (core/meson_options.txt) that build exactly the artifact's
    backends; enable_metal and enable_dnn are feature options, the others booleans."""
    flags = []
    for backend in BACKENDS[1:]:
        on = backend in artifact["backends"]
        if backend in FEATURE_OPTIONS:
            flags.append(f"-Denable_{backend}={'enabled' if on else 'disabled'}")
        else:
            flags.append(f"-Denable_{backend}={'true' if on else 'false'}")
    return [*flags, "-Denable_dnn=disabled"]


def ruby_licence(artifact: dict[str, Any]) -> str:
    ids = sorted(licence_check.licensing.spdx_ids(artifact["licence"]))
    custom = [i for i in ids if i.startswith("LicenseRef-")]
    if custom:
        # Homebrew takes only SPDX list identifiers; the formula says so instead of
        # dropping a licence the files carry.
        return ":cannot_represent"
    return "all_of: [" + ", ".join(f'"{i}"' for i in ids) + "]"


def homebrew_formula(manifest: dict[str, Any], version: str, sha256: str) -> str:
    project, artifact = manifest["project"], manifest["artifacts"]["source"]
    flags = "".join(f'\n      "{flag}",' for flag in configure_flags(artifact))
    return f"""# Generated by scripts/release/distribution.py from packaging/manifest.toml.
class Vmafx < Formula
  desc "{project['summary']}"
  homepage "{project['homepage']}"
  url "{source_url(manifest, version)}"
  sha256 "{sha256}"
  license {ruby_licence(artifact)}

  depends_on "meson" => :build
  depends_on "ninja" => :build

  on_intel do
    depends_on "nasm" => :build
  end

  def install
    args = [{flags}
    ]
    system "meson", "setup", "build", "core", *args, *std_meson_args
    system "meson", "compile", "-C", "build", "--verbose"
    system "meson", "install", "-C", "build"
  end
""" + homebrew_test_block(project["command"])


def homebrew_test_block(command: str) -> str:
    # Its own string: the Meson test-entry-point contract reads one Python string as one
    # command, and the formula's Ruby `test do` after its Meson build lines would read as
    # a raw `meson test` call (core/test/test_meson_secret_env_sanitization.py).
    return f"""
  test do
    assert_match version.to_s, shell_output("#{{bin}}/{command} --version 2>&1")
  end
end
"""


def licence_texts(artifact: dict[str, Any]) -> list[str]:
    ids = sorted(licence_check.licensing.spdx_ids(artifact["licence"]))
    return ["LICENSE", "NOTICE", *(f"LICENSES/{i}.txt" for i in ids)]


def conda_recipe(manifest: dict[str, Any], version: str, sha256: str) -> str:
    project, artifact = manifest["project"], manifest["artifacts"]["source"]
    flags = " ".join(configure_flags(artifact))
    recipe = {
        "context": {"version": version},
        "package": {"name": project["name"], "version": "${{ version }}"},
        "source": {"url": source_url(manifest, "${{ version }}"), "sha256": sha256},
        "build": {
            "number": 0,
            "script": [f"meson setup build core --prefix=$PREFIX --libdir=lib {flags}",
                       "meson compile -C build", "meson install -C build"],
        },
        "requirements": {
            "build": ["${{ compiler('c') }}", "${{ compiler('cxx') }}", "${{ stdlib('c') }}",
                      "meson", "ninja", "pkg-config", {"if": "x86_64", "then": "nasm"}],
        },
        "tests": [{"script": [f"{project['command']} --version"]}],
        "about": {"homepage": project["homepage"], "repository": project["repository"],
                  "license": artifact["licence"], "license_file": licence_texts(artifact),
                  "summary": project["summary"]},
        "extra": {"recipe-maintainers": ["lusoris"]},
    }  # fmt: skip
    head = "# Generated by scripts/release/distribution.py from packaging/manifest.toml.\n"
    body = str(yaml.safe_dump(recipe, sort_keys=False, width=100))
    return head + "schema_version: 1\n\n" + body


def winget_identity(manifest: dict[str, Any], version: str) -> dict[str, Any]:
    project = manifest["project"]
    return {"PackageIdentifier": f"{project['publisher']}.{project['name'].upper()}",
            "PackageVersion": version}  # fmt: skip


def winget_manifests(manifest: dict[str, Any], version: str, sha256: str) -> dict[str, str]:
    """The version, default-locale and installer manifests of the winget channel."""
    project = manifest["project"]
    key = manifest["channels"]["winget"]["artifacts"][0]
    artifact = manifest["artifacts"][key]
    zip_name = file_names(artifact, version)[0]
    identity = winget_identity(manifest, version)
    tail = {"ManifestVersion": WINGET_SCHEMA}
    version_doc = {**identity, "DefaultLocale": "en-US", "ManifestType": "version", **tail}
    locale_doc = {**identity, "PackageLocale": "en-US", "Publisher": project["publisher"],
                  "PackageName": project["name"].upper(), "PackageUrl": project["homepage"],
                  "License": artifact["licence"], "ShortDescription": project["summary"],
                  "ManifestType": "defaultLocale", **tail}  # fmt: skip
    installer = {"Architecture": "x64", "InstallerUrl": release_url(manifest, version, zip_name),
                 "InstallerSha256": sha256.upper()}  # fmt: skip
    installer_doc = {**identity, "InstallerType": "zip", "NestedInstallerType": "portable",
                     "NestedInstallerFiles": [{
                         "RelativeFilePath": f"{zip_name.removesuffix('.zip')}\\build\\tools\\vmaf.exe",
                         "PortableCommandAlias": project["command"]}],
                     "Installers": [installer], "ManifestType": "installer", **tail}  # fmt: skip
    ident = identity["PackageIdentifier"]
    return {f"{ident}.yaml": winget_yaml(version_doc, "version"),
            f"{ident}.locale.en-US.yaml": winget_yaml(locale_doc, "defaultLocale"),
            f"{ident}.installer.yaml": winget_yaml(installer_doc, "installer")}  # fmt: skip


def winget_yaml(document: dict[str, Any], kind: str) -> str:
    schema = f"https://aka.ms/winget-manifest.{kind}.{WINGET_SCHEMA}.schema.json"
    head = "# Generated by scripts/release/distribution.py from packaging/manifest.toml.\n"
    return (
        head
        + f"# yaml-language-server: $schema={schema}\n"
        + str(yaml.safe_dump(document, sort_keys=False, width=200))
    )


def channel_index(manifest: dict[str, Any], name: str, version: str) -> dict[str, Any]:
    """What a channel carries for one version: files, licence, signature and SBOM."""
    channel = manifest["channels"][name]
    entries = []
    for key in channel.get("artifacts", []):
        artifact = manifest["artifacts"][key]
        entries.append({"artifact": key, "kind": artifact["kind"],
                        "platform": artifact["platform"], "abi": artifact["abi"],
                        "backends": artifact["backends"], "licence": artifact["licence"],
                        "files": file_names(artifact, version), "signing": artifact["signing"],
                        "sbom": artifact["sbom"].format(version=version)})  # fmt: skip
    return {"channel": name, "state": channel["state"], "tracking": channel.get("tracking"),
            "version": version, "metadata": channel.get("metadata", []),
            "artifacts": entries}  # fmt: skip


def generate(manifest: dict[str, Any], version: str, digests: dict[str, str]) -> dict[str, str]:
    """Every generated file, by its path below the output directory."""
    if not VERSION.match(version):
        raise DistributionError(f"version {version!r} is not MAJOR.MINOR.PATCH[-PRE]")
    for key in ("source", "winget"):
        if not SHA256.match(digests.get(key, "")):
            raise DistributionError(f"--{key}-sha256 needs 64 lowercase hex digits")
    files = {
        "homebrew/vmafx.rb": homebrew_formula(manifest, version, digests["source"]),
        "conda-forge/recipe.yaml": conda_recipe(manifest, version, digests["source"]),
    }
    for name, text in winget_manifests(manifest, version, digests["winget"]).items():
        files[f"winget/{name}"] = text
    for name in manifest.get("channels", {}):
        index = json.dumps(channel_index(manifest, name, version), indent=2) + "\n"
        files[f"index/{name}.json"] = index
    return files


# -------------------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check", help="fail on any disagreement between manifest and tree")
    writer = commands.add_parser("generate", help="write every channel's metadata")
    writer.add_argument("--version", required=True)
    writer.add_argument("--source-sha256", required=True)
    writer.add_argument("--winget-sha256", required=True)
    writer.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        manifest = load(args.root)
        problems = check(args.root, manifest)
        if problems or args.command == "check":
            for problem in problems:
                print(f"distribution: {problem}", file=sys.stderr)
            print(f"distribution: {len(problems)} problem(s) in {MANIFEST}")
            return 1 if problems else 0
        digests = {"source": args.source_sha256, "winget": args.winget_sha256}
        for path, text in generate(manifest, args.version, digests).items():
            target = args.out / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
            print(f"distribution: wrote {target}")
    except DistributionError as error:
        print(f"distribution: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
