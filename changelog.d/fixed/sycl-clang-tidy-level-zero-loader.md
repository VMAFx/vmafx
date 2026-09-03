- **`Clang-Tidy SYCL (Changed Files, Advisory)` could not configure a SYCL
  build tree, because the Level Zero loader was missing from the runner.**
  `meson setup build-sycl` aborted at
  `cc.find_library('ze_loader', required : true)` with
  `/usr/bin/ld: cannot find -lze_loader` →
  `ERROR: C shared or static library 'ze_loader' not found`, so the job failed
  before `meson compile`, before `gen-sycl-compile-commands.py`, and before a
  single TU reached clang-tidy. This was the next failure uncovered once
  #1227 let the job execute its steps for the first time since the LLVM 22
  bump. The step installs `intel-oneapi-compiler-dpcpp-cpp` and sources
  `setvars.sh --force` before configuring, but oneAPI does not ship the Level
  Zero loader, and the stock `ubuntu-24.04` image carries neither the library
  nor `level_zero/ze_api.h`. The job now installs `libze-dev`
  (`noble/universe`), which provides `libze_loader.so`,
  `level_zero/ze_api.h` and `libze_loader.pc`, and pulls the runtime SONAME
  through its `libze1` dependency. The runtime package alone is not a
  substitute: `cc.find_library` emits a literal `-lze_loader`, which the
  linker resolves against the unversioned `libze_loader.so` symlink only —
  `libze_loader.so.1` is invisible to `-l`.
