---
paths:
  - scripts/ci/tidy_changed_route.py
  - scripts/ci/tests/test_tidy_changed_route.py
invariant: Changed-files tidy jobs skip a file only for lane that measures it or live coverage exception; else fail naming it.
area: tidy
---
<!-- markdownlint-disable MD013 MD060 -->
# Changed-files tidy routing (Q-341, Q-342)

`tidy_changed_route.py changed` feeds `Tidy Changed`, `sycl` feeds `Tidy SYCL`
(`.github/workflows/lint-and-format.yml`).

- `changed`: file with command in CPU database -> linted. Non-header without
  one -> skipped only when `measured_sources` of a
  `tidy-baseline-<lane>.json` lists it (lane named in skip line) or it holds
  live `clang-tidy-coverage` exception (entry and expiry named); else job fails
  naming file. Never add exclusion families to `exclude_untidyable` for files
  some lane measures; routing defers them.
- Header: linted standalone unless no unit including it (directly or through
  headers, quoted includes resolved against own dir, `core/src`,
  `core/include`, `core/test`, `core/src/feature`, `core/tools`) has CPU
  command; then same deferral over those units.
- `sycl`: wrapper parses every file as C++20. Header no C++ source includes
  (`core/src/sycl/vmafx_sycl_internal.h`, C11 atomics) -> replaced by its C
  units with command in `build-sycl`; none -> fail. Never feed C-only header
  alone again: libstdc++ `<atomic>` breaks on `<stdatomic.h>`.
- Selection of `Tidy SYCL` includes `core/src/sycl/*.c` and
  `core/test/test_vmafx_import_sycl*.c`; their commands come from meson's
  database of `build-sycl` (`write-compile-commands.py`), recorded in sycl
  lane `measured_sources` by `tidy-ratchet.py`.
- Planted cases run in `Tooling Tests` (once in CI, ADR-1568) and hook
  `test-tidy-changed-route`; every mode keeps negative case it refuses.
- Test fixture repos run git with every `GIT_*` variable stripped
  (`clean_environment()`): hook exports `GIT_INDEX_FILE`, and inherited, the
  fixture's `git add -A` rewrote the worktree index (seen 2026-10-10).
  `HookEnvironment` plants it.
