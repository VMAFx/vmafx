- **ci(workflows):** Remove dead Vulkan / MoltenVK lane plumbing left
  behind after [ADR-0726](../../docs/adr/0726-drop-vulkan-backend.md)
  dropped the Vulkan backend.
  - `.github/workflows/libvmaf-build-matrix.yml` — drop the
    `matrix.vulkan` Vulkan SDK install step, the `matrix.moltenvk`
    homebrew install step, and the `matrix.moltenvk` Vulkan
    smoke-test step. The matrix never carried a `vulkan: true` or
    `moltenvk: true` row after ADR-0726 (PR #47) landed, so the steps
    were unreachable. Also drop `!matrix.moltenvk` guards from three
    downstream `if:` clauses and simplify `continue-on-error` to
    `matrix.experimental == true`.
  - `.github/workflows/lint-and-format.yml` — remove four copies of
    the `core/src/vulkan/`, `core/src/feature/vulkan/`, and
    `core/test/test_vulkan` `grep -v` exclusion lines from the
    clang-tidy / cppcheck / iwyu file-list pipeline. None of those
    paths exist on disk post-ADR-0726.
  - `scripts/ci/check-dispatch-registry.sh` — drop `vulkan` from
    the backend loop. The existing `[[ ! -d "$src_dir" ]]` short-circuit
    already silently skipped it; dropping the token removes a stale
    tombstone.
