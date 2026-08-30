Bump Ubuntu base images from 24.04 to 26.04 across all Docker files and GitHub Actions
workflows. NVIDIA CUDA images updated to `nvidia/cuda:13.3.0-*-ubuntu26.04`; Intel
oneAPI SYCL image remains on ubuntu24.04 pending upstream availability. GitHub-hosted
`ubuntu-26.04` and `ubuntu-26.04-arm` runner labels are now available (preview) and
wired into all `runs-on:` entries. Dev Dockerfiles renamed: `ubuntu-24.04-*.Dockerfile`
→ `ubuntu-26.04-*.Dockerfile`.
