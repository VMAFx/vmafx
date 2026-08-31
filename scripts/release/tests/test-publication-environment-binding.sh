#!/usr/bin/env bash
# Regression test for protected publication environments and exact tag identities.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"

python3 - "$REPO_ROOT" <<'PY'
from copy import deepcopy
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
workflow_paths = (
    ".github/workflows/supply-chain.yml",
    ".github/workflows/docker-publish-production.yml",
    ".github/workflows/docker-publish-operator-node.yml",
)
SLSA_USES = (
    "slsa-framework/slsa-github-generator/.github/workflows/"
    "generator_generic_slsa3.yml@v2.1.0"
)
SLSA_JOBS = {
    "slsa-provenance": "vmafx-build-provenance.intoto.jsonl",
    "mcp-slsa-provenance": "vmaf-mcp-provenance.intoto.jsonl",
}
VERIFY_TARGETS = {
    "docker-publish-production.yml": (
        '"${{ env.REGISTRY }}/${{ env.IMAGE_NAME }}@'
        '${{ needs.build-cpu.outputs.digest }}"',
        '"${{ env.REGISTRY }}/${{ env.IMAGE_NAME }}@${digest}"',
        '"${{ env.REGISTRY }}/${{ env.IMAGE_NAME }}@'
        '${{ needs.build-server.outputs.digest }}"',
    ),
    "docker-publish-operator-node.yml": (
        '"${{ env.REGISTRY }}/${{ env.OPERATOR_IMAGE }}@'
        '${{ needs.build-operator.outputs.digest }}"',
        '"${{ env.REGISTRY }}/${{ env.SERVER_IMAGE }}@'
        '${{ needs.build-server.outputs.digest }}"',
        '"${{ env.REGISTRY }}/${{ env.NODE_IMAGE }}@'
        '${{ needs.build-node.outputs.digest }}"',
    ),
}


def jobs(text: str) -> dict[str, str]:
    _, jobs_text = text.split("\njobs:\n", maxsplit=1)
    candidate_count = len(re.findall(r"(?m)^  \S[^\n]*:\n", jobs_text))
    matches = list(
        re.finditer(
            r'''(?m)^  (?:"([A-Za-z_][A-Za-z0-9_-]*)"|'''
            r"'([A-Za-z_][A-Za-z0-9_-]*)'|"
            r"([A-Za-z_][A-Za-z0-9_-]*)):\n",
            jobs_text,
        )
    )
    if len(matches) != candidate_count:
        raise AssertionError("workflow contains an unparseable job identifier")
    identifiers = [next(group for group in match.groups() if group) for match in matches]
    if len(set(identifiers)) != len(identifiers):
        raise AssertionError("workflow contains duplicate job identifiers")
    return {
        identifiers[index]: jobs_text[
            match.end() : matches[index + 1].start()
            if index + 1 < len(matches)
            else len(jobs_text)
        ]
        for index, match in enumerate(matches)
    }


def active_text(text: str) -> str:
    return "\n".join(line.split("#", maxsplit=1)[0].rstrip() for line in text.splitlines())


def require_environment(relative_path: str, job: str, block: str, expected: str) -> None:
    if not re.search(rf"(?m)^    environment:\s+{re.escape(expected)}$", block):
        raise AssertionError(
            f"{relative_path}: write-bearing job {job} is not bound to {expected}"
        )


def validate_workflow_permissions(relative_path: str, workflow: str) -> None:
    header, _ = workflow.split("\njobs:\n", maxsplit=1)
    lines = header.splitlines()
    permission_indexes = [
        index for index, line in enumerate(lines) if line.startswith("permissions:")
    ]
    if len(permission_indexes) != 1:
        raise AssertionError(
            f"{relative_path}: expected one workflow-level permissions policy"
        )
    index = permission_indexes[0]
    if lines[index] == "permissions: {}":
        return
    if lines[index] != "permissions:":
        raise AssertionError(f"{relative_path}: scalar workflow permissions are forbidden")

    entries: list[tuple[str, str]] = []
    for line in lines[index + 1 :]:
        if not line.startswith("  "):
            break
        match = re.fullmatch(r"  ([a-z0-9-]+): (read|write|none)", line)
        if match is None:
            raise AssertionError(
                f"{relative_path}: workflow permission line is not parseable: {line}"
            )
        entries.append((match.group(1), match.group(2)))
    if not entries or len(dict(entries)) != len(entries):
        raise AssertionError(f"{relative_path}: workflow permissions are empty or duplicated")
    write_scopes = [scope for scope, access in entries if access == "write"]
    if write_scopes:
        raise AssertionError(
            f"{relative_path}: workflow-level write scopes are forbidden: {write_scopes}"
        )


def validate_slsa_job(relative_path: str, job: str, block: str) -> None:
    expected_permissions = {
        "actions": "read",
        "contents": "read",
        "id-token": "write",
    }
    permissions = dict(
        re.findall(r"(?m)^      ([a-z0-9-]+): (read|write|none)$", block)
    )
    if permissions != expected_permissions:
        raise AssertionError(
            f"{relative_path}: {job} permissions are {permissions}, "
            f"expected {expected_permissions}"
        )
    if f"    uses: {SLSA_USES}\n" not in block:
        raise AssertionError(f"{relative_path}: {job} does not use the pinned generator")
    if "    environment:" in block:
        raise AssertionError(f"{relative_path}: reusable job {job} has invalid environment")
    if "      upload-assets: false\n" not in block:
        raise AssertionError(f"{relative_path}: {job} may upload a release asset")
    if "upload-tag-name:" in block:
        raise AssertionError(f"{relative_path}: {job} retains direct release upload")
    expected_name = SLSA_JOBS[job]
    if f"      provenance-name: {expected_name}\n" not in block:
        raise AssertionError(f"{relative_path}: {job} provenance name drifted")


def validate_attachment(block: str) -> None:
    _, steps_text = block.split("\n    steps:\n", maxsplit=1)
    step_count = len(re.findall(r"(?m)^      - ", steps_text))
    if step_count != 3:
        raise AssertionError(
            f"supply-chain.yml attachment job must have exactly 3 steps, got {step_count}"
        )
    if block.count("actions/download-artifact@") != 1:
        raise AssertionError(
            "supply-chain.yml attachment job must use download-artifact exactly once"
        )
    if block.count("softprops/action-gh-release@") != 1:
        raise AssertionError(
            "supply-chain.yml attachment job must use action-gh-release exactly once"
        )

    needs_match = re.search(
        r"(?m)^    needs:\n(?P<body>(?:      - [a-z0-9-]+\n)+)", block
    )
    if needs_match is None:
        raise AssertionError("supply-chain.yml attachment dependencies are not parseable")
    needs = {
        line.strip().removeprefix("- ")
        for line in needs_match.group("body").splitlines()
    }
    missing_needs = set(SLSA_JOBS) - needs
    if missing_needs:
        raise AssertionError(
            f"supply-chain.yml attachment does not wait for {sorted(missing_needs)}"
        )

    download_matches = list(
        re.finditer(
        r"(?ms)^      - uses: actions/download-artifact@[a-f0-9]{40}.*?"
        r"(?=^      - |\Z)",
        block,
        )
    )
    if len(download_matches) != 1:
        raise AssertionError(
            "supply-chain.yml attachment job must have one artifact download"
        )
    download = download_matches[0].group(0)
    if not (
        "          path: dist/\n" in download
        or re.search(r"(?m)^        with: \{ path: dist/ \}$", download)
    ):
        raise AssertionError("supply-chain.yml does not download all artifacts under dist/")
    if re.search(r"(?m)^          (name|pattern):", download):
        raise AssertionError("supply-chain.yml filters the all-artifact download")
    if re.search(r"(?m)^          merge-multiple:\s+true$", download):
        raise AssertionError("supply-chain.yml flattens artifact directories")

    require_step_match = re.search(
        r"(?ms)^      - name: Require every release asset and matching signature\n"
        r".*?(?=^      - |\Z)",
        block,
    )
    release_step_matches = list(
        re.finditer(
            r"(?ms)^      - uses: softprops/action-gh-release@[a-f0-9]{40}.*?"
            r"(?=^      - |\Z)",
            block,
        )
    )
    if require_step_match is None or len(release_step_matches) != 1:
        raise AssertionError("supply-chain.yml attachment steps are not parseable")
    if not (
        download_matches[0].start()
        < require_step_match.start()
        < release_step_matches[0].start()
    ):
        raise AssertionError("supply-chain.yml attachment steps are out of order")
    require_step = require_step_match.group(0)
    release_step = release_step_matches[0].group(0)
    required_match = re.search(
        r"(?ms)^          required=\(\n(?P<body>.*?)^          \)\n",
        require_step,
    )
    files_match = re.search(
        r"(?m)^          files: \|\n"
        r"(?P<body>(?:            \S[^\n]*(?:\n|\Z))+)",
        release_step,
    )
    if required_match is None or files_match is None:
        raise AssertionError("supply-chain.yml attachment lists are not parseable")
    required = {line.strip() for line in required_match.group("body").splitlines()}
    files = {line.strip() for line in files_match.group("body").splitlines()}
    expected_files = {
        "dist/release-artifacts/*",
        "dist/sbom/*",
        "dist/signatures/*",
        "dist/mcp-dist/*",
        "dist/mcp-signatures/*",
        *(
            f"dist/{name}/{name}"
            for name in SLSA_JOBS.values()
        ),
    }
    if files != expected_files:
        raise AssertionError(
            "supply-chain.yml release asset set drifted: "
            f"expected {sorted(expected_files)}, got {sorted(files)}"
        )
    for name in SLSA_JOBS.values():
        artifact_path = f"dist/{name}/{name}"
        if artifact_path not in required:
            raise AssertionError(f"supply-chain.yml does not require {artifact_path}")
        if artifact_path not in files:
            raise AssertionError(f"supply-chain.yml does not attach {artifact_path}")


def validate_docker_identities(relative_path: str, text: str) -> None:
    if "--certificate-identity-regexp" in text or "@.*" in text:
        raise AssertionError(f"{relative_path}: broad certificate identity remains")
    expected_identity = (
        "https://github.com/VMAFx/vmafx/.github/workflows/"
        f"{Path(relative_path).name}@refs/tags/${{PUBLISH_TAG}}"
    )
    lines = text.splitlines()
    commands: list[list[str]] = []
    index = 0
    while index < len(lines):
        if re.match(r"^\s+cosign verify \\$", lines[index]):
            command = [lines[index]]
            while command[-1].rstrip().endswith("\\"):
                index += 1
                if index >= len(lines):
                    raise AssertionError(f"{relative_path}: truncated cosign verifier")
                command.append(lines[index])
            commands.append(command)
        index += 1

    if len(commands) != 3:
        raise AssertionError(
            f"{relative_path}: expected 3 cosign verifiers, got {len(commands)}"
        )
    expected_issuer = (
        "--certificate-oidc-issuer "
        "https://token.actions.githubusercontent.com \\"
    )
    expected_targets = VERIFY_TARGETS[Path(relative_path).name]
    for verifier_index, (command, expected_target) in enumerate(
        zip(commands, expected_targets, strict=True), start=1
    ):
        normalized = [line.strip() for line in command]
        expected_command = [
            "cosign verify \\",
            "--certificate-identity \\",
            f'"{expected_identity}" \\',
            expected_issuer,
            expected_target,
        ]
        if normalized != expected_command:
            raise AssertionError(
                f"{relative_path}: verifier {verifier_index} command drifted; "
                f"expected {expected_command}, got {normalized}"
            )


def validate(texts: dict[str, str]) -> None:
    parsed = {
        relative_path: jobs(active_text(text))
        for relative_path, text in texts.items()
    }
    for relative_path, blocks in parsed.items():
        if "    environment:" in blocks["validate-release"]:
            raise AssertionError(f"{relative_path}: read-only validation is environment-gated")
        workflow = active_text(texts[relative_path])
        validate_workflow_permissions(relative_path, workflow)
        if re.search(
            r"(?m)^    permissions:[ \t]+(?!\{\}[ \t]*$)\S",
            workflow,
        ):
            raise AssertionError(f"{relative_path}: scalar permissions policy is forbidden")

        for job, block in blocks.items():
            write_scopes = re.findall(
                r"(?m)^      ([a-z0-9-]+): (?:write|\"write\"|'write')$",
                block,
            )
            if job in SLSA_JOBS:
                validate_slsa_job(relative_path, job, block)
                continue
            if not write_scopes:
                continue
            expected = (
                "pypi-publish" if job == "mcp-publish-pypi" else "release-publish"
            )
            require_environment(relative_path, job, block, expected)

    supply_blocks = parsed[workflow_paths[0]]
    validate_attachment(supply_blocks["attach-to-release"])
    for relative_path in workflow_paths[1:]:
        validate_docker_identities(relative_path, active_text(texts[relative_path]))


def expect_rejected(name: str, texts: dict[str, str]) -> None:
    try:
        validate(texts)
    except AssertionError:
        print(f"PASS: rejected fixture with {name}")
        return
    raise AssertionError(f"broken fixture unexpectedly passed: {name}")


texts = {
    relative_path: (root / relative_path).read_text(encoding="utf-8")
    for relative_path in workflow_paths
}
validate(texts)

production_path = workflow_paths[1]
bad_identity = deepcopy(texts)
expected_identity = (
    "https://github.com/VMAFx/vmafx/.github/workflows/"
    "docker-publish-production.yml@refs/tags/${PUBLISH_TAG}"
)
bad_identity[production_path] = bad_identity[production_path].replace(
    expected_identity,
    "https://github.com/evil/other/.github/workflows/other.yml@refs/tags/${PUBLISH_TAG}",
    1,
)
expect_rejected("one wrong certificate identity", bad_identity)

misbound_identities = deepcopy(texts)
identity_clause_pattern = re.compile(
    rf'(?m)^[ \t]+--certificate-identity \\\n'
    rf'[ \t]+"{re.escape(expected_identity)}" \\\n'
)
identity_clauses = list(identity_clause_pattern.finditer(misbound_identities[production_path]))
if len(identity_clauses) != 3:
    raise AssertionError("identity-clause fixture anchors are absent")
first_clause = identity_clauses[0].group(0)
second_clause = identity_clauses[1]
misbound = (
    misbound_identities[production_path][: second_clause.start()]
    + misbound_identities[production_path][second_clause.end() :]
)
misbound_identities[production_path] = (
    misbound[: identity_clauses[0].end()]
    + first_clause
    + misbound[identity_clauses[0].end() :]
)
expect_rejected("certificate identities detached from verifiers", misbound_identities)

unsafe_cosign = deepcopy(texts)
issuer_line = (
    "            --certificate-oidc-issuer "
    "https://token.actions.githubusercontent.com \\\n"
)
if issuer_line not in unsafe_cosign[production_path]:
    raise AssertionError("unsafe-cosign fixture anchor is absent")
unsafe_cosign[production_path] = unsafe_cosign[production_path].replace(
    issuer_line,
    issuer_line + "            --insecure-ignore-tlog \\\n",
    1,
)
expect_rejected("unsafe extra cosign verifier flag", unsafe_cosign)

write_all = deepcopy(texts)
write_all[production_path], replacements = re.subn(
    r"(?m)^    environment: release-publish\n"
    r"    permissions:\n"
    r"(?:      [a-z0-9-]+: (?:read|write)(?:\s+#.*)?\n)+",
    "    permissions: write-all\n",
    write_all[production_path],
    count=1,
)
if replacements != 1:
    raise AssertionError("write-all fixture mutation did not apply")
expect_rejected("unprotected permissions: write-all", write_all)

quoted_writes = deepcopy(texts)
quoted_writes[production_path], environment_removals = re.subn(
    r"(?m)^    environment: release-publish\n",
    "",
    quoted_writes[production_path],
)
quoted_writes[production_path], quoted_write_count = re.subn(
    r"(?m)^(      [a-z0-9-]+): write(?P<comment>\s+#.*)?$",
    r'\1: "write"\g<comment>',
    quoted_writes[production_path],
)
if environment_removals == 0 or quoted_write_count == 0:
    raise AssertionError("quoted-write fixture mutation did not apply")
expect_rejected("unprotected quoted write scopes", quoted_writes)

top_level_write = deepcopy(texts)
top_level_write[production_path], replacements = re.subn(
    r"(?m)^permissions:\n(?:  [a-z0-9-]+: (?:read|write|none)\n)+",
    "permissions:\n  contents: write\n",
    top_level_write[production_path],
    count=1,
)
if replacements != 1:
    raise AssertionError("top-level write fixture mutation did not apply")
expect_rejected("workflow-level contents write", top_level_write)

underscore_job = deepcopy(texts)
job_anchor = "\n  smoke-test:\n"
if underscore_job[production_path].count(job_anchor) != 1:
    raise AssertionError("underscore-job fixture anchor is not unique")
underscore_job[production_path] = underscore_job[production_path].replace(
    job_anchor,
    """
  unprotected_writer:
    runs-on: ubuntu-latest
    permissions:
      packages: "write"
    steps:
      - run: echo unsafe
"""
    + job_anchor,
    1,
)
expect_rejected("underscore-named unprotected writer", underscore_job)

commented_provenance = deepcopy(texts)
native_path = (
    "dist/vmafx-build-provenance.intoto.jsonl/"
    "vmafx-build-provenance.intoto.jsonl"
)
last_index = commented_provenance[workflow_paths[0]].rfind(f"            {native_path}")
if last_index < 0:
    raise AssertionError("provenance fixture mutation anchor is absent")
commented_provenance[workflow_paths[0]] = (
    commented_provenance[workflow_paths[0]][:last_index]
    + f"            # {native_path}"
    + commented_provenance[workflow_paths[0]][last_index + 12 + len(native_path) :]
)
expect_rejected("commented-out provenance attachment", commented_provenance)

extra_protected_step = deepcopy(texts)
release_step_anchor = "      - uses: softprops/action-gh-release@"
if extra_protected_step[workflow_paths[0]].count(release_step_anchor) != 1:
    raise AssertionError("extra-protected-step fixture anchor is not unique")
extra_protected_step[workflow_paths[0]] = extra_protected_step[
    workflow_paths[0]
].replace(
    release_step_anchor,
    """      - name: Tamper with verified release assets
        run: printf 'tampered' >> dist/release-artifacts/vmaf
"""
    + release_step_anchor,
    1,
)
expect_rejected("extra protected release step", extra_protected_step)

extra_release_asset = deepcopy(texts)
release_files_anchor = "          files: |\n"
if extra_release_asset[workflow_paths[0]].count(release_files_anchor) != 1:
    raise AssertionError("extra-release-asset fixture anchor is not unique")
extra_release_asset[workflow_paths[0]] = extra_release_asset[workflow_paths[0]].replace(
    release_files_anchor,
    "          files: |\n            dist/unverified/*\n",
    1,
)
expect_rejected("unverified extra release asset", extra_release_asset)

print("PASS: every publication write is environment-bound")
print("PASS: SLSA provenance is attached only by the protected release writer")
print("PASS: container signatures require the exact published tag identity")
PY
