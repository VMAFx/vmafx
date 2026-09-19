- **The GPU build lanes are warning-free again, and four CI test helpers no longer
  flake on a loaded runner.** A glob written inside a block comment
  (`core/src/feature/hip/*.c`) opens a nested comment, so 15 files warned under
  `-Wcomment` on every HIP and CUDA build — 28 of the HIP lane's 53 warnings, plus one
  on the Metal header where the same glob also pointed at the pre-ADR-0700
  `libvmaf/src/` path. `__HIP_PLATFORM_AMD__` now comes from the build in one place
  rather than a reserved-identifier `#define` in eight sources
  ([ADR-1263](docs/adr/1263-hip-platform-macro-single-source.md)). The 10-second
  subprocess caps in four `scripts/ci/` test modules, which are hang detectors rather
  than timing assertions, are raised to 120 s after a CI run blew them under contention
  and the `TimeoutExpired` read as a real failure. `.gitignore` now also matches
  `.workingdir` / `.workingdir2` as symlinks, not only as directories.
