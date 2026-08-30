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

import os
import sys

_REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
_PYTHON_DIR = os.path.join(_REPO_ROOT, "python")
if _PYTHON_DIR not in sys.path:
    sys.path.insert(0, _PYTHON_DIR)
