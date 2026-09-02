# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Compatibility shim — the vmaf Python package was relocated to
# compat/python-vmaf/ as part of the VMAFX repo-layout move (ADR-0700).
# This shim ensures legacy callers using `import vmaf` (with python/ on
# sys.path) continue to work.
#
# Strategy: insert compat/ into sys.path, then re-import vmaf. The
# compat/vmaf symlink (-> python-vmaf/) makes `import vmaf` resolve to
# the real package. See also: compat/vmaf -> compat/python-vmaf/.
#
# New code should add compat/ to PYTHONPATH directly.

import importlib
import os
import sys

_repo_root = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
_compat_dir = os.path.join(_repo_root, "compat")
if _compat_dir not in sys.path:
    sys.path.insert(0, _compat_dir)

# Remove this shim from sys.modules so the next import resolves from
# compat/ via the compat/vmaf -> python-vmaf/ symlink.
del sys.modules[__name__]
importlib.import_module(__name__)
