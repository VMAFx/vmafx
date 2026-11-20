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
  consistency. `renovate.json` tracks the NuGet pins through the `tracked.cpu`
  and `tracked.gpu` anchors (`nuget` datasource); a bump changes only the
  anchor, and the guard refuses a tracked version without a size/SHA-256
  entry. **On sync**: new ORT NuGet download sites must use
  `scripts/ci/ort_nuget_install.py --package cpu|gpu` and declare a pinned
  `ORT_VERSION`.
