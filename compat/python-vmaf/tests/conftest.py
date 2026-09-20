# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# conftest.py for compat/python-vmaf/tests/
#
# Ensures that ``import vmaf`` resolves to the compat/vmaf shim package when
# pytest is invoked from a working directory that does not already have python/
# on sys.path.  The pyproject.toml [tool.pytest.ini_options] pythonpath list is
# the primary mechanism; this file is a belt-and-suspenders guard for editors
# and direct ``pytest compat/python-vmaf/tests/`` invocations.
from __future__ import annotations

import sys
from pathlib import Path

_REPO_ROOT = str(Path(__file__).resolve().parents[3])
_PYTHON_DIR = str(Path(_REPO_ROOT).joinpath("python"))
if _PYTHON_DIR not in sys.path:
    sys.path.insert(0, _PYTHON_DIR)
