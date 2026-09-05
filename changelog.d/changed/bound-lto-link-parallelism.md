- Build: `b_lto_threads=4` joins the project `default_options`, so a link uses at
  most four LTO partitions instead of every core (GCC's plain `-flto` means
  `-flto=auto`). Parallel builds no longer push a 32-core workstation past load
  190. `docs/development/build-flags.md` now states the real `b_lto` default
  (`true`) and documents the override (ADR-1172).
