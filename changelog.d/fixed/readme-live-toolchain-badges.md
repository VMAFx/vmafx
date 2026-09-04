- Restored the README toolchain badges (Go, Rust edition, Python, C, C++,
  CUDA, ROCm) that the README overhaul dropped, and made them live: Go reads
  `go.mod`, Python reads `pyproject.toml`, Rust reads `Cargo.toml`, CUDA reads
  the `Dockerfile` base-image pin and ROCm reads `dev/Containerfile`, all via
  shields.io dynamic endpoints, so they can no longer drift from the tree (the
  old static badges said Go 1.26 and C11 while the tree was at 1.27.1 and C23).
  A live release badge was added alongside.
