# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# ai/sidecar/tests/test_quickstart_contract.py — regression tests for standalone
# quick-start documentation contract and writable checkpoint configuration.
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.
# ADR-1309: socket path ownership and owner-only mode.

from __future__ import annotations

import pathlib
import re
import tempfile
import unittest.mock as mock

import pytest

from ai.sidecar import online_trainer as ot_mod


class TestQuickstartDocumentationContract:
    """Fail-closed regression tests for the standalone sidecar documentation contract."""

    @classmethod
    def _doc_path(cls) -> pathlib.Path:
        repo_root = pathlib.Path(__file__).resolve().parents[3]
        return repo_root / "docs" / "ai" / "sidecar-online-training.md"

    def test_doc_exists_and_readable(self) -> None:
        doc = self._doc_path()
        assert doc.is_file(), f"Documentation missing at {doc}"

    def test_quickstart_configures_writable_checkpoint_and_cleanup(self) -> None:
        doc_text = self._doc_path().read_text(encoding="utf-8")

        # Find the bash block under 'Deployment status'
        deployment_match = re.search(
            r"## Deployment status.*?(```bash\n.*?\n```)",
            doc_text,
            re.DOTALL,
        )
        assert deployment_match is not None, "Deployment status quickstart code block missing"
        snippet = deployment_match.group(1)

        # Must allocate private runtime dir
        assert 'runtime_dir="$(mktemp -d)"' in snippet
        # Must include shell-safe cleanup trap
        assert "trap 'rm -rf \"$runtime_dir\"' EXIT INT TERM" in snippet
        # Must restrict runtime permissions
        assert 'chmod 700 "$runtime_dir"' in snippet
        # Must allocate a writable checkpoint directory
        assert 'mkdir -p "$runtime_dir/checkpoints"' in snippet
        # Must configure socket override
        assert 'VMAFX_SIDECAR_SOCKET="$runtime_dir/vmafx-sidecar.sock"' in snippet
        # Must explicitly configure writable checkpoint directory
        assert 'VMAFX_SIDECAR_CHECKPOINT_DIR="$runtime_dir/checkpoints"' in snippet
        # Must invoke module
        assert "python -m ai.sidecar.online_trainer" in snippet

        # Document must explicitly explain the /mnt container default and PermissionError risk
        assert "/mnt/vmafx-models/online" in doc_text
        assert "PermissionError" in doc_text

    def test_no_standalone_snippet_omits_checkpoint_dir(self) -> None:
        doc_text = self._doc_path().read_text(encoding="utf-8")
        # Ensure no bash snippet runs online_trainer without VMAFX_SIDECAR_CHECKPOINT_DIR
        for block in re.findall(r"```bash\n(.*?)\n```", doc_text, re.DOTALL):
            if "ai.sidecar.online_trainer" in block:
                assert (
                    "VMAFX_SIDECAR_CHECKPOINT_DIR=" in block
                ), "Standalone invocation snippet lacks VMAFX_SIDECAR_CHECKPOINT_DIR override"
                assert "trap " in block, "Standalone invocation snippet lacks cleanup trap"


class TestCheckpointDirectoryContract:
    """Functional tests verifying checkpoint directory configuration and failure modes."""

    def test_default_checkpoint_dir_targets_container_mount(self) -> None:
        assert ot_mod._CHECKPOINT_DIR == "/mnt/vmafx-models/online"

    def test_unwritable_checkpoint_dir_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            ro_parent = pathlib.Path(td) / "readonly_root"
            ro_parent.mkdir(parents=True, exist_ok=True)
            ro_parent.chmod(0o500)
            target_ckpt = ro_parent / "unwritable_ckpt"

            try:
                with (
                    mock.patch("ai.sidecar.online_trainer._load_base_model"),
                    mock.patch("ai.sidecar.online_trainer.SGDEMATrainer"),
                    pytest.raises(PermissionError),
                ):
                    ot_mod.OnlineTrainer(n_features=8, checkpoint_dir=str(target_ckpt))
            finally:
                ro_parent.chmod(0o700)

    def test_writable_checkpoint_dir_creates_and_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            target_ckpt = pathlib.Path(td) / "checkpoints"
            assert not target_ckpt.exists()

            with (
                mock.patch("ai.sidecar.online_trainer._load_base_model"),
                mock.patch("ai.sidecar.online_trainer.SGDEMATrainer"),
            ):
                trainer = ot_mod.OnlineTrainer(n_features=8, checkpoint_dir=str(target_ckpt))
                assert target_ckpt.is_dir()
                assert trainer._checkpoint_dir == target_ckpt
