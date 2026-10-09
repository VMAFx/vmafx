- **The changed-files clang-tidy jobs defer to the lane that measures a file instead of failing
  on it (Q-341, Q-342).** In `Tidy Changed`, a changed file without a command in the CPU build is
  skipped only when a lane's `measured_sources` lists it, or when a live `clang-tidy-coverage`
  exception holds it; the skip line names the lane or the expiry. Any other such file fails the
  job by name. `Tidy SYCL` now also lints the SYCL C sources and the device-frame import tests,
  and lints a C-only header through its C includers instead of parsing it as C++.
  Details: [CI](docs/development/ci.md).
