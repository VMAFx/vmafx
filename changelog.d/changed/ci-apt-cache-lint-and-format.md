<!-- markdownlint-disable MD041 -->
- **Apt installation steps in `lint-and-format.yml` now cache package archives and carry per-step timeout caps.**
  Package requirements for `clang-tidy`, `clang-tidy-ratchet`, `clang-tidy-sycl`, and `cppcheck` are declared under `.github/apt/` with 5-minute install caps sized at 1.25 times measured runs.
  The `cppcheck` job limit is raised to 45 minutes to absorb step caps and execution variance, preventing slow mirror stalls.
