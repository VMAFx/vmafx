- CI: the `build.yml` legs (`Linux Intel LLVM`, `macOS Clang+Metal`,
  `Windows MSVC+CUDA (full)`) now run when a pull request changes files under
  `python/` or `compat/`, their lock files or the fixture prune script. They
  followed the `c_core` selector, so such a pull request passed these checks
  without running `test_decorator_extended.py` (whose only Windows run is there)
  or the macOS `tox -c python`. The new impact selector `build_work` in
  `.github/ci-impact.json` covers these inputs.
