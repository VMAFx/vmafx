- Fixed required-gate failure in `make tidy-ratchet LANE=sycl` where `scripts/ci/clang-tidy-sycl.sh`
  was passed as a relative executable and rejected by `safe_subprocess.py`'s bare-or-absolute contract.
  `Makefile` now anchors the wrapper path to `$(CURDIR)`, and `tidy-ratchet.py` resolves relative
  `--clang-tidy` paths against the repository root and working directory.
