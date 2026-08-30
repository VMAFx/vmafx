#!/usr/bin/env python3

"""
VMAF - Video Multimethod Assessment Fusion

VMAF is a perceptual video quality assessment algorithm developed by Netflix.
VMAF Development Kit (VDK) is a software package that contains the VMAF algorithm implementation,
as well as a set of tools that allows a user to train and test a custom VMAF model.
"""

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
        with open(os.path.join(COMPAT_VMAF, "__init__.py")) as fh:
            for line in fh:
                if line.startswith("__version__"):
                    return line.strip().rpartition(" ")[2].replace('"', "")

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
            self._extensions[0].include_dirs = [
                numpy.get_include(),
                "compat",
                "../core/src",
            ]

        return self._extensions

    def __iter__(self):
        return iter(self.extensions)

    def __contains__(self, value):
        return value in self.extensions

    def __len__(self):
        return len(self.extensions)


setup(
    name="vmaf",
    version=get_version(),
    author="Zhi Li",
    author_email="zli@netflix.com",
    description="Video Multimethod Assessment Fusion",
    long_description=open(os.path.join(PYTHON_PROJECT, "README.rst")).read(),
    long_description_content_type="text/x-rst",
    url="https://github.com/Netflix/vmaf",
    package_dir={"vmaf": COMPAT_VMAF_REL},
    packages=["vmaf", "vmaf.tools", "vmaf.core", "vmaf.script"],
    package_data={"vmaf": ["py.typed"]},
    include_package_data=True,
    install_requires=[
        "numpy>=2.5.2",
        "scipy>=1.18.1",
        "matplotlib>=3.11.1",
        "pandas>=3.0.5",
        "scikit-learn>=1.9.0",
        "scikit-image>=0.26.0",
        "h5py>=3.16.0",
        "sureal>=0.9.0",
        "dill>=0.4.1",
        "PyWavelets>=1.9.0",
        "python-slugify>=8.0.4",
        "libsvm-official>=3.37",
    ],
    entry_points={
        "console_scripts": [
            "run_cleaning_cache=vmaf.script.run_cleaning_cache:main",
            "run_psnr=vmaf.script.run_psnr:main",
            "run_result_assembly=vmaf.script.run_result_assembly:main",
            "run_testing=vmaf.script.run_testing:main",
            "run_toddnoiseclassifier=vmaf.script.run_toddnoiseclassifier:main",
            "run_vmaf=vmaf.script.run_vmaf:main",
            "run_vmaf_cross_validation=vmaf.script.run_vmaf_cross_validation:main",
            "run_vmaf_training=vmaf.script.run_vmaf_training:main",
        ],
    },
    ext_modules=LazyExtensions(),
)
