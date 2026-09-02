- `renovate.json`: add `customManagers` entry + `customDatasources.rocm`
  block so Renovate auto-tracks future ROCm apt-repo releases. Queries
  `https://repo.radeon.com/rocm/apt/` via the `html` datasource; matches
  both the `ARG ROCM_VER=` line and the `rocm/apt/<ver>/` URL path in
  `dev/Containerfile`. ROCm bumps are grouped under a `manual-review` label
  with `automerge: false` due to KFD ioctl ABI risk. The current pin (7.2.3)
  is unchanged — it is the current production-stable release; 7.13.0 is a
  technology preview with no apt packages. See [ADR-0604](docs/adr/0604-rocm-renovate-manager.md).
