## nvcc host-compiler flags keyed on the build machine (2026-11-05)

- `core/src/meson.build`: the CUDA host-compiler strict-FP spelling and the
  win32 pthread shim include path now test `build_machine.system()`, as the
  `-ccbin` selection does since #3293. Fork-only: upstream's nvcc section has
  no host-compiler discovery, so both hunks are dropped on a sync rather than
  merged. `core/test/test_strict_fp_compiler_args.py` substitutes the build and
  host machines into the fixture INDEPENDENTLY and adds a build=linux /
  host=windows cross row that asserts the Unix spelling, plus a static check
  that pins both machine gates: a later rebase that reverts either hunk to
  `host_machine.system()` fails its own test instead of silently reddening the
  CUDA lanes.
