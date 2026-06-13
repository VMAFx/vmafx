- **C++ standard bumped to C++23** in `core/meson.build` `default_options`
  (`cpp_std=c++11` → `cpp_std=c++23`, ADR-1003). The project-wide default was
  inconsistent with the many files already compiled at C++23 via per-target
  `override_options` (Wave 1–9 C++ migration, ADR-0708/0727). Any new `.cpp`
  file added directly to `libvmaf_sources` now compiles at C++23 by default,
  removing the need for boilerplate isolated static libs. Pre-existing
  `override_options: ['cpp_std=c++23']` on isolated libs are redundant but
  harmless; cleanup is deferred to a follow-up sweep PR.
- **Fix**: `test_feature_collector_coverage` (fast suite) had been
  link-failing since introduction — its internal-helper calls required
  `feature_collector.cpp` to be compiled directly into the test, not just
  linked through `libvmaf` (which contains `feature_collector.c` with static
  helpers). Fixed using the same pattern as `test_predict`.
