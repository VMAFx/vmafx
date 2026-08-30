- Removed committed merge-conflict markers from `.github/workflows/libvmaf-build-matrix.yml`
  (CUDA and SYCL Windows build steps) and `.github/workflows/security-scans.yml` (CodeQL
  Python no-op build step). Both were introduced by commit `0c494cca05` (post-merge-train
  sweep) and caused `check-yaml` pre-commit hook failures. Retained HEAD side in all three
  conflicts: `core\build` paths (ADR-0700 rename) and the explicit no-op build step
  (suppresses CodeQL autobuild.sh on Python-only scans).
