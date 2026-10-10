#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Both dev-MCP services mount the corpus on its own, read-only.

``/workspace`` is a read-only bind of the repository. When ``.corpus`` in the
checkout is a symlink to a dataset elsewhere on the host, the container sees a
link to a path it does not have. The corpus therefore needs its own bind, with
the host resolving the source (``VMAFX_CORPUS_DIR``, default ``./.corpus``) and
Compose creating an empty directory where there is no corpus.
"""

from __future__ import annotations

import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

COMPOSE = Path(__file__).resolve().parents[3] / "dev" / "docker-compose.yml"
SERVICES = ("dev-mcp", "smoke-probe-cron")
SOURCE = "${VMAFX_CORPUS_DIR:-./.corpus}"
TARGET = "/workspace/.corpus"


def corpus_mount_faults(document: object) -> list[str]:
    """One line per service that lacks the read-only corpus bind."""
    services = document.get("services", {}) if isinstance(document, dict) else {}
    faults: list[str] = []
    for name in SERVICES:
        volumes = (services.get(name) or {}).get("volumes") or []
        mounts = [v for v in volumes if isinstance(v, dict) and v.get("target") == TARGET]
        if len(mounts) != 1:
            faults.append(f"{name}: {len(mounts)} mounts at {TARGET}, want 1")
            continue
        mount = mounts[0]
        wanted = {"type": "bind", "source": SOURCE, "read_only": True}
        wrong = {k: mount.get(k) for k, v in wanted.items() if mount.get(k) != v}
        if (mount.get("bind") or {}).get("create_host_path") is not True:
            wrong["bind.create_host_path"] = (mount.get("bind") or {}).get("create_host_path")
        if wrong:
            faults.append(f"{name}: {wrong}")
    return faults


class CorpusMount(unittest.TestCase):
    def test_both_services_mount_the_corpus(self) -> None:
        document = yaml.safe_load(COMPOSE.read_text(encoding="utf-8"))
        self.assertEqual(corpus_mount_faults(document), [])

    def test_missing_writable_or_hard_coded_mounts_are_refused(self) -> None:
        probe_volumes: list[dict[str, object]] = [
            {"type": "bind", "source": ".", "target": "/workspace"}
        ]
        planted = {
            "services": {
                "dev-mcp": {
                    "volumes": [
                        {"type": "bind", "source": ".", "target": "/workspace", "read_only": True},
                        {"type": "bind", "source": SOURCE, "target": TARGET, "read_only": False},
                    ]
                },
                "smoke-probe-cron": {"volumes": probe_volumes},
            }
        }
        faults = corpus_mount_faults(planted)
        self.assertEqual(len(faults), 2, faults)
        self.assertIn("read_only", faults[0])
        self.assertIn("bind.create_host_path", faults[0])
        self.assertIn("0 mounts", faults[1])
        probe_volumes.append(
            {
                "type": "bind",
                "source": "/mnt/data/corpus/vmafx",
                "target": TARGET,
                "read_only": True,
                "bind": {"create_host_path": True},
            }
        )
        self.assertIn("source", corpus_mount_faults(planted)[1])


if __name__ == "__main__":
    unittest.main()
