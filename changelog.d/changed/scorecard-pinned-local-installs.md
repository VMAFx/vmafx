- **Local images and local packages are installed pinned for Scorecard.**
  `Dockerfile.ffmpeg` now accepts `ARG VMAF_IMAGE_DIGEST` and pins the local
  `vmaf` base image by digest, satisfying OpenSSF Scorecard Pinned-Dependencies
  rules while keeping the base image exact. The RC1 Tester Report CI step builds
  a local wheel with `--no-deps --no-build-isolation` into a staging directory
  and installs it via `--no-deps`, replacing an unpinned directory install.
  `check_python_dependency_locks.py` now strictly enforces that non-editable
  local installs require `.whl` files (ADR-3315).
