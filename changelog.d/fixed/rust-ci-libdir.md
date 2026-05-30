- **Rust CI — libvmaf link path fix.** `rust-ci.yml` now passes
  `--libdir lib` to meson so libvmaf installs to `/usr/local/lib/`
  instead of `/usr/local/lib/x86_64-linux-gnu/` (Ubuntu's multiarch
  default). The downstream `cargo test` linker only searches
  `/usr/local/lib`, so the multiarch path previously broke linking
  with `unable to find library -lvmaf` even after a successful build
  + install.
