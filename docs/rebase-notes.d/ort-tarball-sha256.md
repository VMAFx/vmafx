## ONNX Runtime tarballs verified by sha256 (2026-10-16)

- Every site fetching the ONNX Runtime C/C++ native release archive (`dev/Containerfile`,
  `.github/workflows/go-ci.yml`, `build.yml`, `libvmaf-build-matrix.yml`, and
  `tests-and-quality-gates.yml`) pairs `ORT_VERSION` with a pinned `ORT_SHA256`
  and verifies the digest via `sha256sum --check --strict` before extraction
  (ADR-3334).
- Cached archives in `tests-and-quality-gates.yml` re-verify checksum on every
  run, removing the cache entry and failing immediately on mismatch.
- `scripts/ci/check_ort_tarball_pins.py` statically enforces presence of
  `ORT_SHA256`, verification ordering before `tar -x`, and cross-site digest
  consistency. `renovate.json` updates version and digest in locked pairs via
  `github-release-attachments`. **On sync**: new ORT tarball download sites must
  declare `ORT_SHA256` and verify with `sha256sum --check --strict`.
