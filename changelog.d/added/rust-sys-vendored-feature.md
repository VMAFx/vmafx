<!-- Copyright 2026 Lusoris -->
<!-- SPDX-License-Identifier: EUPL-1.2 -->
<!-- markdownlint-disable MD041 -->

- Add `vendored` feature to `vmafx-sys` to compile and statically link the
  CPU-only C core from `OUT_DIR` using Meson and Ninja. System mode requires
  both `libvmaf` and `libvmafx` development files installed; vendored mode links
  statically so built binaries require no runtime library installation.
  When neither library is found and `vendored` is off, a named build error
  directs callers to set `LIBVMAF_PREFIX`, install development files, or enable
  `vendored`. The vendored source directory defaults to `core/` and can be
  overridden via `VMAFX_VENDOR_DIR`.
