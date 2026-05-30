### Changed

- AGENTS.md: rebrand titles and tree diagrams in all 23 `core/**/AGENTS.md`
  files from `libvmaf/` to `core/` to match the ADR-0700 directory rename.
  Install-path references (`include/libvmaf/`, `core/include/libvmaf/`,
  `<libvmaf/*.h>` includes) are preserved verbatim — they describe the
  install layout, not the source layout. References to the upstream
  Netflix `libvmaf/src/metadata_handler.c` path are also preserved because
  they describe Netflix's tree, not the fork's.
