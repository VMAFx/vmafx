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
- Install CUDA on the Windows CI legs from NVIDIA's redistributable archives
  via `scripts/ci/install-cuda-toolkit.ps1`, replacing the
  `cuda_<version>_windows_network.exe` step. NVIDIA publishes no such installer
  for CUDA 13.4 — every shape of that path returns 404 while the 13.3.1 one
  returns 200 — but `redistrib_13.4.1.json` does list `windows-x86_64`
  artifacts for every component the build needs, so the componentised
  redistributables are the distribution that actually exists for 13.4. The
  script unpacks `cuda_nvcc`, `cuda_cudart`, `cuda_crt`, `libnvvm` and
  `visual_studio_integration` into one `CUDA_PATH`: the same subset the network
  installer was asked for. Component versions come from the manifest at run
  time, so the tree never spells them (13.4.1 ships `cuda_nvcc` 13.4.59).
- Shrink the CUDA coordinated pin from sixteen sites in seven spellings to ten
  in four. Both install scripts read `CUDA_VERSION` from `build-config.env`
  instead of repeating it, so the `Jimver/cuda-toolkit` input, `$cudaVersion`,
  `$cudaMajorMinor` and `CUDA_PATH_V<major>_<minor>` no longer appear in any
  workflow — the last three were duplicated verbatim across `build.yml` and
  `libvmaf-build-matrix.yml`. `check-cuda-pin-lockstep.py` drops those four
  shapes rather than keeping them matching nothing; the residual sweep still
  rejects any CUDA release literal a shape did not claim, so re-introducing one
  fails as an unrecognised pin.
- Teach `scripts/ci/check-cuda-pin-lockstep.py` the
  `CUDA_PATH_V<major>_<minor>` spelling. It is the one site shape where the
  CUDA release is part of a variable *name* rather than a value, so it was
  invisible to the gate: two workflow sites still read `CUDA_PATH_V13_3` after
  a `--write` had moved every other spelling, which would have exported a
  13.4 toolkit under the 13.3 release's name.
