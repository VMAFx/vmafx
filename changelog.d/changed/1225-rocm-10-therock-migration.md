- **ROCm 10.0.0 across every HIP consumer, installed from digest-pinned
  container images** (ADR-1225). AMD froze the `repo.radeon.com/rocm/apt/`
  channel at 7.2.4 when ROCm moved to the "TheRock" build/release system at
  7.14 — `apt/7.14` and `apt/10.0.0` both 404, the manylinux channel stops at
  `rocm-rel-7.2.4`, and the TheRock wheel index carries only 7.14.0 alphas —
  so the fork now takes ROCm from `rocm/dev-ubuntu-24.04:10.0.0-full`
  (digest-pinned) instead. `dev/Containerfile`,
  `docker/Dockerfile.production-gpu` and `docker/Dockerfile.node` copy a
  pruned `/opt/rocm` out of that image; the two CI HIP lanes use the new
  `scripts/ci/install-rocm-from-image.sh`, which streams the image's
  `/opt/rocm` out of the registry (peak disk ~5.5 GB rather than the 29 GB a
  `docker pull` would need on a runner that already carries CUDA and oneAPI).
  Verified on AMD `gfx1036` under Linux 7.2.3: HIP tests 19 Ok / 0 Fail and
  an end-to-end HIP score of `45.315104`, bit-identical to the CPU reference.
- **`HSA_OVERRIDE_GFX_VERSION=10.3.0` removed from
  `dev/docker-compose.yml`.** ROCm 10 supports `gfx1036` natively, so the
  alias onto `gfx1030` that ROCm 6.x/7.x needed is now actively wrong — it
  would map the agent to `gfx1030` while meson compiles `gfx1036` code
  objects for the arch `rocm_agent_enumerator` reports.
- **`node-rocm` image ships the complete HIP runtime closure.** ROCm 10's
  `libamdhip64.so` links `librocprofiler-register`, `librocm_kpack`,
  `libamd_comgr` and the bundled `libLLVM` / `libclang-cpp` plus a
  `rocm_sysdeps` bundle; the previous flat copy of `libamdhip64.so*` +
  `libhsa-runtime64.so*` would have produced an image whose every HIP binary
  died at load. The stage now copies the verified 397 MB closure with its
  `$ORIGIN`-relative directory layout intact.
- **The README ROCm badge follows the image pin.** It scraped
  `ARG ROCM_VER=` out of `dev/Containerfile`, which ADR-1225 removes; it now
  reads the version out of the digest-pinned
  `rocm/dev-ubuntu-24.04:<version>-full` reference in the same file, so it
  keeps reporting a live value instead of going blank.
