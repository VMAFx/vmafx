- Install CUDA on the Linux CI legs from NVIDIA's own apt repository via
  `scripts/ci/install-cuda-toolkit.sh` instead of the `Jimver/cuda-toolkit`
  action. The action ships its own table of installable CUDA releases, so it
  decided which versions the fork could pin: its newest release (v0.2.36) tops
  out at 13.3.1 and has no 13.4.x entry, which left the CUDA 13.4.1 bump
  unmergeable in both directions. The script reads `CUDA_APT_PACKAGE` from
  `build-config.env`, derives the distro tag from `/etc/os-release`, and
  installs the same `nvcc` + `cudart-dev` subset the action was configured
  for, so runner time and disk are unchanged. CUDA releases are now pinnable
  the day NVIDIA publishes them, and the CI legs install from the same source
  as `dev/Containerfile`. See ADR-1300.
- Teach `scripts/ci/check-cuda-pin-lockstep.py` the
  `CUDA_PATH_V<major>_<minor>` spelling. It is the one site shape where the
  CUDA release is part of a variable *name* rather than a value, so it was
  invisible to the gate: two workflow sites still read `CUDA_PATH_V13_3` after
  a `--write` had moved every other spelling, which would have exported a
  13.4 toolkit under the 13.3 release's name.
