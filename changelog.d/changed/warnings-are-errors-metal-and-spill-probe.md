- **The macOS Metal leg is gated, and the SYCL spill probe no longer fills the build log.** Meson
  1.12 names `-lc++` twice on every link that carries an Objective-C++ object (224 ld64
  warnings per run); the Metal links now tell ld64 duplicate libraries are expected
  (`-Wl,-no_warn_duplicate_libraries`, reason in `core/src/metal/meson.build`), so the leg takes
  `werror: true`. The AOT compile of `scratch_check.cpp`, whose deliberate register-spill kernel
  makes the device compiler warn on every target, runs through `core/src/sycl/run_captured.py`,
  which prints that output only when the compile fails
  ([ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md)).
