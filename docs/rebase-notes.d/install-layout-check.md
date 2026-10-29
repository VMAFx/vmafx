## Installed Linux SDK layout check (2026-10-28)

Keep `scripts/ci/check_install_layout.py`, its tooling-suite pytest coverage,
the `install-layout-check` Make target, and the release-guide install section
together. SONAME majors come from `core/meson.build`; engine symbol versions
reuse `core/test/check_exported_symbols.py` and the generated
`core/src/vmafx_symbols.txt`. Preserve DESTDIR-only pkg-config and loader paths,
`--no-rebuild`, the build-RUNPATH lookup for dependencies outside the
loader path (refusing a directory with a `libvmaf*` library), and the `--plant-unversioned` failure control. The real pytest
smoke requires `VMAF_BUILD_DIR`; parser fixtures always run.
