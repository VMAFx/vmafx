- The native Linux `vmaf` CLI attached to a release runs next to the
  downloaded `libvmaf.so*` files without `LD_LIBRARY_PATH`. The
  `v1.0.0-rc.1` CLI kept Meson's build-tree RUNPATH `$ORIGIN/../src`, so it
  found `libvmaf.so.3` only when `LD_LIBRARY_PATH` pointed at the download
  directory. The release build now sets the staged CLI's RUNPATH to exactly
  `$ORIGIN`, and the release gate runs the CLI without `LD_LIBRARY_PATH` and
  rejects any other RUNPATH.
