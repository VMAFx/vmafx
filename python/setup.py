#!/usr/bin/env python3

"""
VMAF - Video Multimethod Assessment Fusion

VMAF is a perceptual video quality assessment algorithm developed by Netflix.
VMAF Development Kit (VDK) is a software package that contains the VMAF algorithm implementation,
as well as a set of tools that allows a user to train and test a custom VMAF model.
"""

import ast
import os

from setuptools import setup

PYTHON_PROJECT = os.path.dirname(os.path.abspath(__file__))

# Real package location after ADR-0700 repo-layout move. We need two forms:
#   - COMPAT_VMAF_REL: relative to PYTHON_PROJECT, used in setup() kwargs
#     (setuptools rejects absolute paths in package_dir on macOS Clang).
#   - COMPAT_VMAF: absolute, used for file I/O (get_version, cythonize).
# Use the `compat/vmaf` symlink rather than the hyphenated `compat/python-vmaf/`
# directory so Cython can derive a valid module name from the .pyx path
# (Cython rejects `python-vmaf.core.adm_dwt2_cy` because hyphens are
# illegal in Python module names; via the symlink it sees
# `vmaf.core.adm_dwt2_cy`).
COMPAT_VMAF_REL = os.path.join("..", "compat", "vmaf")
COMPAT_VMAF = os.path.normpath(os.path.join(PYTHON_PROJECT, COMPAT_VMAF_REL))


def get_version():
    """Version from vmaf __init__ (reads from the real package location)."""
    try:
        with open(os.path.join(COMPAT_VMAF, "__init__.py"), encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("__version__"):
                    _, separator, value = line.partition("=")
                    if separator:
                        version = ast.literal_eval(value.split("#", 1)[0].strip())
                        if isinstance(version, str):
                            return version

    except Exception:
        pass

    return "0.0-dev"


class LazyExtensions(list):
    _extensions = None

    @property
    def extensions(self):
        if self._extensions is None:
            import numpy
            from Cython.Build import cythonize

            # Use the relative path so the resulting Extension's source list
            # stays relative to setup.py (setuptools rejects absolute paths
            # in Extension.sources on macOS Clang the same way it does for
            # package_dir).
            self._extensions = cythonize(
                [os.path.join(COMPAT_VMAF_REL, "core", "adm_dwt2_cy.pyx")],
                compiler_directives={"language_level": "3"},
            )
            # python/compat/ contains a stub config.h that disables SIMD
            # dispatch (the SIMD .c files are not compiled into this extension).
            # The directly included core sources use both private headers under
            # core/src and public libvmaf headers under core/include.
            self._extensions[0].include_dirs = [
                numpy.get_include(),
                "compat",
                "../core/src",
                "../core/include",
            ]
            # aligned_malloc / aligned_free moved from core/src/mem.c to
            # core/src/mem.cpp when the C++23 twins were wired in (#1133).
            # The .pyx used to text-include the .c; a C++23 TU cannot be
            # included into this C module, so compile it as a real source.
            # language="c++" only selects the C++ driver for the link step —
            # the Cython-generated .c is still compiled as C by extension.
            self._extensions[0].sources.append(os.path.join("..", "core", "src", "mem.cpp"))
            # The .pyx also text-includes adm.c. Its fail-closed score helpers
            # call vmaf_log(), so the extension must carry the implementation;
            # otherwise the wheel links but import fails with an undefined
            # vmaf_log symbol (first exposed by the hosted ARM64 lane).
            self._extensions[0].sources.append(os.path.join("..", "core", "src", "log.c"))
            self._extensions[0].language = "c++"

        return self._extensions

    def __iter__(self):
        return iter(self.extensions)

    def __contains__(self, value):
        return value in self.extensions

    def __len__(self):
        return len(self.extensions)


# Everything expressible in PEP 621 lives in python/pyproject.toml, which
# setuptools treats as authoritative: a key passed here that `[project]`
# neither declares nor lists as `dynamic` raises `_MissingDynamic`, and on the
# Coverage Gate runner that is fatal rather than advisory —
# `python3 setup.py --version` exited 1 on `authors` and `scripts`. What stays
# is only what `[project]` cannot express: the out-of-tree package directory
# (the sources live in compat/python-vmaf/, ADR-0700), and the lazily-built
# Cython extension. `version` stays because `[project].dynamic = ["version"]`
# is exactly how a dynamic version is supplied.
setup(
    version=get_version(),
    package_dir={"vmaf": COMPAT_VMAF_REL},
    packages=["vmaf", "vmaf.tools", "vmaf.core", "vmaf.script"],
    package_data={"vmaf": ["py.typed"]},
    include_package_data=True,
    ext_modules=LazyExtensions(),
)
