- Add tech-stack badges to README grouped by category: CI/build/test,
  version pins (Go 1.26, Rust edition 2021, Python 3.14+, C11, C++11,
  CUDA 13.2, ROCm 7.2), GPU/SIMD capabilities, and distribution/community
  (GHCR container link). Closes ADR-1000.
- Bump Go version pin: `go.mod` minimum and `go-ci.yml` toolchain pin
  aligned at 1.26.4 (was go.mod 1.25.0 / go-ci.yml 1.23). Resolves a
  two-major-version CI/module drift that allowed regressions to accumulate
  silently.
