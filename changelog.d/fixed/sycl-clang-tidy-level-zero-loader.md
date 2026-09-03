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
- **`Clang-Tidy SYCL (Changed Files, Advisory)` then failed to link its build
  tree, because LTO was left on under a mismatched linker plugin.** With the
  loader in place `meson setup` succeeds and the job reaches `meson compile`
  for the first time, where every test binary dies with
  `bfd plugin: LLVM gold plugin has failed to create LTO module: Unknown
  attribute kind (102) (Producer: 'Intel.oneAPI.DPCPP.Compiler_2026.1.1'
  Reader: 'LLVM 17.0.6')`. `core/meson.build` sets `b_lto=true` as a project
  `default_option`, so linking runs LTO through the stock `ubuntu-24.04`
  binutils gold plugin — LLVM 17.0.6 — which cannot read bitcode emitted by
  the oneAPI DPC++ compiler. The SYCL legs of `libvmaf-build-matrix.yml`
  already pass `-Db_lto=false` for exactly this reason. The job now does the
  same; it only needs codegen outputs and a `compile_commands.json`, so it has
  nothing to gain from LTO.
