## Pinned local image and local package installs (2026-10-14)

- `Dockerfile.ffmpeg` replaces `FROM vmaf:latest` with `ARG VMAF_IMAGE_DIGEST`
  and `FROM vmaf@sha256:${VMAF_IMAGE_DIGEST}` to satisfy OpenSSF Scorecard
  dependency pinning. The base image validation gate in
  `scripts/ci/check-container-image-references.py` binds this digest-based FROM.
- `.github/workflows/tests-and-quality-gates.yml` RC1 Tester Report step replaces
  `pip install ./tools/rc1-tester` with `pip wheel` into `dist/rc1-wheel` followed
  by `pip install --no-deps dist/rc1-wheel/*.whl` (ADR-3315).
- `scripts/ci/check_python_dependency_locks.py` refuses non-editable plain-directory
  and sdist installs, requiring `.whl` files with `--no-deps`.
- **On sync**: upstream Netflix has neither `Dockerfile.ffmpeg` nor
  `check-container-image-references.py`. Any new local package installations in
  CI or Docker recipes must build and install `.whl` archives with `--no-deps`
  or use `-e` with both `--no-deps` and `--no-build-isolation`.
