---
paths:
  - scripts/ci/check-cuda-pin-lockstep.py
  - scripts/ci/install-cuda-toolkit.*
  - scripts/ci/tests/test_cuda_pin_single_source.py
  - scripts/ci/tests/test_install_cuda_toolkit.py
invariant: One CUDA release = seven literals owned by `build-config.env` `CUDA_VERSION`; never narrow residual sweep.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# CUDA coordinated pin (ADR-1285)

One CUDA release = seven literals across two files. Authority =
`build-config.env` `CUDA_VERSION`; that file also records apt package
series, release-review latch, and exact toolkit/nvcc/cudart Debian versions.
`check-cuda-pin-lockstep.py` checks all seven, `--write` derives only apt
series and OCI-description spellings, and residual sweep fails on any CUDA
release literal in unrecognised spelling.
Never narrow sweep to silence new site: teach gate its shape and
owner in same change, or site drifts. `--write` must never touch
`CUDA_VERSION` or exact apt metadata. Component build numbers are not
function of marketing release; refresh them from NVIDIA's live redist
manifest and Ubuntu Packages index. `CUDA_APT_LOCK_RELEASE` deliberately stays
outside Renovate ownership, so bot bump fails until that review happens.
shared installer must keep exact `package=version` apt operands, subsequent
`dpkg-query` validation, and all three builder/runtime/full modes. Its fake-host
contract suite has dedicated `test-install-cuda-toolkit` pre-commit hook,
which required Pre-Commit workflow runs. Renovate resolves `CUDA_VERSION` through
`custom.nvidia-cuda-redist`: official HTML index plus exact
`redistrib_X.Y.Z.json` extractor. Never restore old `nvidia/cuda` package
group. custom feed has no timestamps, so its narrowly matched rule stays
timestamp-optional, manual-review, and non-automerge. Fixture:
`tests/test_cuda_pin_single_source.py`, run by
`test-base-image-single-source` hook.
