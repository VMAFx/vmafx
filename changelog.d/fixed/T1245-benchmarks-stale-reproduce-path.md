- **`docs/benchmarks.md` reproduce block no longer names a path that stopped
  existing at ADR-0700.** The documented build step was
  `meson setup core/build libvmaf`; the meson source root has been `core/` since
  the repo layout change, so the documented procedure aborted with
  "Neither source directory 'core/build' nor build directory None contain a
  build file meson.build". Corrected to `meson setup core/build core`.
