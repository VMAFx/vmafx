- Renovate now proposes a CUDA release move as one pull request instead of an
  unmergeable one. `nvidia/cuda` version updates are grouped as
  `CUDA release (coordinated pin)` together with `CUDA_VERSION`, the
  `Jimver/cuda-toolkit` inputs and the Windows installer versions, so the ten
  sites Renovate can rewrite land in one branch. Previously only the two image
  pins moved, and `scripts/ci/check-base-image-single-source.sh` rejects an
  image tag that disagrees with `CUDA_VERSION`, so such a pull request (#1487)
  could never go green on its own.
- New gate `scripts/ci/check-cuda-pin-lockstep.py` (also `make cuda-pin-sync`,
  and a `Pre-Commit` hook) checks all sixteen places one CUDA release is named
  and fails on a CUDA release literal in any spelling it does not recognise.
  Five of those sites had no drift check before: the two `$cudaMajorMinor`
  series literals on the Windows legs, the `cuda-toolkit-NN-N` apt names in
  `CUDA_APT_PACKAGE` and `dev/Containerfile`, and the
  `org.opencontainers.image.description` label that publishes
  `"VMAFX production CUDA 13.3.1 runtime"` on the CUDA runtime image.
  `--write` derives all five from `CUDA_VERSION`. See ADR-1285.
