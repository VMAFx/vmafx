- Rust bindings: `vmafx-sys`'s build script now refuses a libvmaf header that
  lacks a libvmaf name the `vmafx-sys` and `vmafx` crates use (for example an
  older install left under `/usr/local`). It names the header path and the
  missing names, instead of the build failing later inside `vmafx` with
  unresolved imports
  ([Rust guide](docs/development/rust.md#an-older-install-under-the-prefix), #2786).
