- Added missing SPDX-License-Identifier declarations across 387 clean source
  and header files in accordance with ADR-1250 and repository provenance,
  skipping 131 files with baselined debt, 5 vendored Pelorus mirror paths,
  and 2 files undergoing concurrent review. Extended `scripts/ci/check-copyright.sh` and the `check-copyright`
  pre-commit hook to enforce valid SPDX license identifiers on languages named
  in ADR-1250 (C, C++, CUDA, Go, Python), backed by positive, negative, and
  boundary unit test suite in `scripts/ci/tests/test_check_copyright.py`.
