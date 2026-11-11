# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The bounded runner must keep the test interpreter's virtual environment (ADR-3532)."""

from __future__ import annotations

import os
import stat
import sys
import unittest
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from scripts.lib import safe_subprocess  # noqa: E402

# Bound = max(60, 10x the slowest measured run): the interpreter starts in 0.05 s.
LAUNCH_TIMEOUT_S = 60
LS = "/bin/ls"


def _directory_listing(directory: Path) -> str:
    """Return `ls -led` for a link's directory on macOS, which shows extended ACL entries."""
    if sys.platform != "darwin":
        return ""
    listing = safe_subprocess.run(
        [LS, "-led", str(directory)],
        allowed_executables=(LS,),
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=LAUNCH_TIMEOUT_S,
    )
    return f"{listing.stdout}{listing.stderr}".strip()


def _launch_chain_report(executable: Path) -> str:
    """Describe every followed link, its owner and its directory's rights for a failed launch."""
    if not hasattr(os, "getuid"):
        return f"no os.getuid on {sys.platform}: the runner launches the resolved file"
    mask = os.umask(0o022)
    os.umask(mask)
    lines = [f"uid={os.getuid()} umask={mask:04o} executable={executable}"]
    for link in safe_subprocess._executable_links(executable):
        directory = link.parent.stat()
        retargetable = safe_subprocess._directory_allows_link_retargeting(directory, link.parent)
        lines.append(
            f"link={link} -> {link.readlink()} owner={link.lstat().st_uid} "
            f"dir_owner={directory.st_uid} dir_mode={stat.filemode(directory.st_mode)} "
            f"dir_retargetable={retargetable} {_directory_listing(link.parent)}"
        )
    return "\n".join(lines)


class InterpreterLaunchTest(unittest.TestCase):
    """Tests that spawn `sys.executable` need its packages, so the launch must keep the venv."""

    def test_launch_keeps_the_interpreter_prefix(self) -> None:
        completed = safe_subprocess.run(
            [sys.executable, "-c", "import sys; print(sys.prefix)"],
            allowed_executables=[sys.executable],
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=LAUNCH_TIMEOUT_S,
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            completed.stdout.strip(), sys.prefix, _launch_chain_report(Path(sys.executable))
        )

    def test_report_names_each_link_of_a_chain(self) -> None:
        report = _launch_chain_report(Path(sys.executable))

        self.assertIn(f"executable={sys.executable}", report)
        if hasattr(os, "getuid"):
            links = safe_subprocess._executable_links(Path(sys.executable))
            self.assertEqual(report.count("\nlink="), len(links))

    def test_report_without_getuid_says_resolved_launch(self) -> None:
        getuid = getattr(os, "getuid", None)
        if getuid is not None:
            del os.getuid
            self.addCleanup(setattr, os, "getuid", getuid)

        self.assertIn("launches the resolved file", _launch_chain_report(Path(sys.executable)))


if __name__ == "__main__":
    unittest.main()
