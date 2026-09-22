- **`python/pyproject.toml` now declares the setuptools floor its own
  metadata requires, so building the `vmaf` Python package no longer
  depends on the ambient setuptools being recent enough.** Since
  ADR-1236 the package declares `[project].license` as a PEP 639 SPDX
  expression (a bare string). Only setuptools 77.0.1 and newer parse
  that form; every earlier release rejects it with
  `configuration error: project.license must be valid exactly by one
  definition` and aborts, so `python setup.py --version`,
  `python setup.py build_ext` and `make cythonize` all failed outright
  on any environment carrying an older setuptools — including
  Ubuntu 24.04, which ships 68.1.2 in `/usr/lib/python3/dist-packages`
  where `pip install --user` leaves it untouched. `[build-system]
  requires` now pins `setuptools>=77.0.1`, `make cythonize`'s
  `cythonize-deps` installs the same floor into the venv it then runs
  `setup.py` against (no PEP 517 isolation there to fetch a newer
  backend on its own), and the two CI lanes that run `python/test/`
  against the ambient interpreter install it alongside the
  requirements. `python/test/setup_metadata_test.py` gained a guard
  that fails if the declared floor ever stops excluding a setuptools
  that cannot read the metadata, and its `setup.py` helper now reports
  the subprocess's stderr instead of a bare exit status.
