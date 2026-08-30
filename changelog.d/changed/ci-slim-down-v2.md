### CI matrix slimmed to 1 build per OS + state-of-the-art sanitizers (ADR-0710)

`libvmaf-build-matrix.yml` (15 build rows post-ADR-0689) is replaced by
`build.yml` with three matrix rows:

- **Linux** — GCC + ALL backends (CUDA + SYCL + Vulkan + HIP + CPU + DNN); runs
  the full meson test suite including Netflix golden assertions.
- **macOS** — Apple Clang + CPU + Metal scaffold; runs meson suite + tox.
- **Windows** — MSVC + CPU + CUDA (build-only); runs CPU unit tests.

Sanitizer jobs moved from `tests-and-quality-gates.yml` into a new
`sanitizers.yml`:

- `Sanitizers — ASan + UBSan (PR gate)` — combined `-fsanitize=address,undefined`
  on every non-draft PR (replaces the 3-way per-PR matrix).
- `Sanitizers — TSan (master push)` — thread-sanitizer fires only on master push.
- `Fuzz — * (nightly)` — libFuzzer + ASan against all harnesses nightly.

`Cppcheck (Whole Project)` removed from lint-and-format.yml; clang-tidy provides
a superset of its signal. `Required Checks Aggregator` updated accordingly.

Estimated per-PR runner-time reduction: ~70% versus the pre-ADR-0689 baseline.
