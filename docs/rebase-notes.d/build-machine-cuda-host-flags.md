## nvcc host-compiler flags keyed on the build machine (2026-11-05)

- `core/src/meson.build`: the CUDA host-compiler strict-FP spelling and the
  win32 pthread shim include path now test `build_machine.system()`, as the
  `-ccbin` selection does since #3293. Fork-only: upstream's nvcc section has
  no host-compiler discovery, so both hunks are dropped on a sync rather than
  merged. `core/test/test_strict_fp_compiler_args.py` normalises the fixture on
  either spelling, so a later rebase that reverts one of them fails that test
  instead of silently reddening the CUDA lanes.
