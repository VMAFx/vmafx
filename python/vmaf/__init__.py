# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Compatibility shim — the vmaf Python package was relocated to
# compat/python-vmaf/ as part of the VMAFX repo-layout move (ADR-0700).
# This shim ensures legacy callers using `import vmaf` (with python/ on
# sys.path) continue to work.
#
# Strategy: load compat/vmaf/__init__.py directly by file location and
# replace this module in sys.modules with the result. The shim never
# re-enters the import system for its own name, so the redirect cannot
# depend on — or be defeated by — the order of sys.path entries.
#
# The previous strategy (insert compat/ into sys.path, drop this module
# from sys.modules, re-import `vmaf`) was order-dependent and deadlocked
# every multiprocessing spawn child: `spawn.prepare()` restores a sys.path
# in which python/ already precedes compat/, so the "already on sys.path"
# guard skipped the insert, the re-import resolved back to this file, and
# the child died with RecursionError before releasing the semaphore its
# parent was blocked on. See ADR-1292.
#
# New code should add compat/ to PYTHONPATH directly.

import importlib.util
import os
import sys

_repo_root = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
_compat_dir = os.path.join(_repo_root, "compat")
_real_pkg_dir = os.path.join(_compat_dir, "vmaf")
_real_init = os.path.join(_real_pkg_dir, "__init__.py")

if not os.path.isfile(_real_init):
    raise ImportError(
        f"vmaf compatibility shim: the real package is missing at {_real_init}. "
        "compat/vmaf is a symlink to compat/python-vmaf; a checkout without "
        "symlink support has to add compat/python-vmaf to sys.path itself."
    )

# Keep compat/ importable for legacy callers that reach for sibling packages
# there. Submodule resolution no longer depends on it: the spec below pins
# vmaf.__path__ to the real package directory.
if _compat_dir not in sys.path:
    sys.path.insert(0, _compat_dir)

_spec = importlib.util.spec_from_file_location(
    __name__, _real_init, submodule_search_locations=[_real_pkg_dir]
)
if _spec is None or _spec.loader is None:
    raise ImportError(f"vmaf compatibility shim: cannot build a spec for {_real_init}")

_module = importlib.util.module_from_spec(_spec)
# Publish before executing so circular imports inside the real package see the
# module being initialised, and so the import machinery picks this object up
# instead of the shim it was loading.
sys.modules[__name__] = _module
_spec.loader.exec_module(_module)
