- `Makefile`: corrected stale `LIBVMAF_DIR := libvmaf` variable to
  `core` following the ADR-0700 directory rename; `BUILD_DIR` and
  `DEBUG_DIR` (used by `build`, `test`, `debug`, `install`, `clean`,
  `lint-c`, `test-netflix-golden`, `test-fast`, `test-sanitizers`,
  `coverage`) now resolve to `core/build` and `core/debug` instead
  of the non-existent `libvmaf/build` and `libvmaf/debug`.
- `Makefile`: `test-sanitizers`, `coverage`, and `test-fast` now
  invoke meson and ninja via the venv variables (`$(MESON)`,
  `$(NINJA)`, `$(MESON_SETUP)`) and `PATH="$(VENV)/bin:$$PATH"`,
  consistent with all other build targets.
- `Makefile`: added missing `.PHONY` declarations for
  `docs-fragments-check` and `docs-fragments-write`.
- `Makefile` `help` target: added entries for
  `docs-fragments-check` and `docs-fragments-write`.
