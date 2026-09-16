- **Linted the SYCL backend: 60 fewer clang-tidy warnings, no behaviour
  change.** ADR-1142 puts GPU code under the same standards as
  everything else, and the SYCL lane had never been swept.
  `readability-braces-around-statements` and
  `readability-isolate-declaration` are now clean across all 23 SYCL
  translation units. Both are purely syntactic, so
  they cannot move a score; verified anyway by rebuilding with `icpx`
  and running the full SYCL suite on the Arc A380 — 195/195 pass,
  including every cross-backend parity test.
  Two other checks were tried and **reverted as unsound on SYCL**:
  `misc-const-correctness` and `readability-non-const-parameter` both
  add `const` to variables and parameters that SYCL kernel bodies
  mutate through `sycl::atomic_ref`, which clang-tidy cannot see.
  Applying their own fixes produces a tree that does not compile — so
  the pre-existing `NOLINT(misc-const-correctness)` suppressions in
  these kernels are correct, and the prose already on them ("mutated via
  atomic_ref — clang-tidy cannot see") is now backed by a measurement.
