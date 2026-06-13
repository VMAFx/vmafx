### dev-mcp container: ubuntu:26.04 + CUDA 13.2 (ADR-0573)

- Base image bumped from `ubuntu:24.04` to `ubuntu:26.04` (Resolute Raccoon,
  April 2026 LTS) in `dev/Containerfile`.
- CUDA bumped from the unversioned `cuda-toolkit` (tracking 13.0/13.1) to
  `cuda-toolkit-13-2` (13.2.0). CUDA 13.2 resolves the glibc 2.43 `rsqrt`
  double-declaration conflict introduced by Ubuntu 26.04's glibc. A two-repo
  strategy is used: ubuntu2604 keyring + ubuntu2404 toolkit packages.
- ROCm 7.2.3 retained via `repo.radeon.com` `noble` apt channel (resolute
  channel not yet published by AMD).
- `mesa-va-drivers` replaced by `mesa-libgallium` (the Ubuntu 26.04 successor
  package that provides `mesa-va-drivers` + `va-driver` virtual packages).
- Python updated from 3.12 to 3.14 in the container; `requires-python` widened
  to `<3.15` in `tools/vmaf-tune/pyproject.toml` and `ai/pyproject.toml`.
- NumPy ceiling in `python/requirements.txt` raised from `<2.0.0` to `<3.0.0`
  (floor raised to `>=2.2.0` which has Python 3.14 wheels).
- CI CUDA pin bumped from `13.0.0` to `13.2.0` in
  `.github/workflows/libvmaf-build-matrix.yml` (Linux and Windows legs).
- Intel oneAPI, Intel NEO compute-runtime (26.18.38308.1), and Level Zero
  (1.28.6) are unchanged.
- Supersedes an earlier failed attempt at the 26.04 bump.
