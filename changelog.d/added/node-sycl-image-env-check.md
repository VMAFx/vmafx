- **Smoke-test `vmafx-node:<tag>-sycl` for `VMAFX_BACKEND=sycl` on publish.**
  `docker-publish-operator-node.yml` now runs
  `scripts/release/smoke-published-image.sh` with kind `node-sycl` on the
  published `-sycl` digest after signature verification. It inspects
  `Config.Env` of the pulled image and refuses a tag lacking the exact
  `VMAFX_BACKEND=sycl` entry (missing or set to another value), checks the
  `/usr/local/bin/vmafx-node` entrypoint, `--version` and the FUSE mount, and
  prints the GPU skip reason instead of passing SYCL scoring silently
  (`scripts/release/tests/test-smoke-published-image.sh`).
