- Fix Windows CUDA configuration when `vswhere` cannot find MSVC but `cl.exe`
  is on `PATH`: NVCC and MSVC include discovery now share the resolved compiler
  path. Add a Meson configure regression for discovery, fallback and missing tools.
