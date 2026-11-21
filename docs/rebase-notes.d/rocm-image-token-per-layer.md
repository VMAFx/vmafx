## ROCm image layer pull tokens (2026-11-20)

`scripts/ci/install-rocm-from-image.sh` fetches a registry pull token before
the manifest and again before each layer attempt. On sync, retain the token
refresh inside the retry loop and its shared `fetch_token` implementation.
`scripts/ci/tests/test_install_rocm_from_image.py` checks token exhaustion,
token endpoint failure and retries with a fresh token.
