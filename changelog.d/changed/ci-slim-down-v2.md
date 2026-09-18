### CI: `build.yml` and `sanitizers.yml` added alongside the existing matrix (ADR-0710)

`build.yml` adds one all-in-one build per OS, next to
`libvmaf-build-matrix.yml` rather than in place of it:

- **`Linux Intel LLVM`**: icx/icpx with CUDA, SYCL, HIP and DNN; runs the meson
  suite and the HIP smoke test.
- **`macOS Clang+Metal`**: Apple Clang with CPU and Metal; runs the meson
  suite and tox.
- **`Windows MSVC+CUDA`**: MSVC with CPU and CUDA; builds and runs the CPU
  unit tests.

`sanitizers.yml` adds a combined `Sanitizers ASan+UBSan` job on pull requests,
`Sanitizers TSan` on pushes to master and nightly libFuzzer runs.

None of these jobs is a required check. `libvmaf-build-matrix.yml`, `Cppcheck`
and the required `Sanitizers (address|thread|undefined)` matrix are unchanged;
ADR-1259 records the matrix as it runs.
